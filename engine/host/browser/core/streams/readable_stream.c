/* ReadableStream — the Streams Standard §4.2, §4.3, §4.4 and §4.5.
 *
 * §4.3 IS A MIXIN OF TWO MEMBERS AND NOT THE READER — `ReadableStreamGenericReader` declares `closed` and
 * `cancel()`, and NOTHING else. `read()` and `releaseLock()` belong to §4.4 The ReadableStreamDefaultReader
 * class and §4.5 The ReadableStreamBYOBReader class, which INCLUDE that mixin; every `Generic*` abstract
 * operation is §4.9.3 Readers. A citation here that said §4.3 for any of those resolved to a section that
 * defines them nowhere, which reads as checked and is not.
 *
 * WHY IT IS BUILT NOW. It is the largest absent component by a wide margin: 74 corpus failures name it
 * directly, 19 more name TextDecoderStream, and every `response.body.getReader()` is another. It is also the
 * one place this engine's own architecture is most on display — a reader's `read()` settles a promise, and
 * settling is the page's code, so it is a machine like every other member that touches one.
 *
 * WHAT IS HERE. The stream, the default reader, §4.5's default controller with its PULL loop, and a source that
 * is the HOST'S BYTES. §4.9.4's ReadableStreamDefaultControllerCallPullIfNeeded reacts to the promise the page's
 * `pull` returned by calling `pull` AGAIN and, on rejection, by erroring the controller — so its reactions are
 * MACHINES (rejecting a parked read request settles a promise) that know their controller only by CAPTURE, and
 * they are attached with PerformPromiseThen rather than a `.then` read because that is what the spec performs.
 * Both of those primitives are quickjs's (JS_NewStepClosure, JS_PerformPromiseThen), added for this.
 *
 * WHAT IS ELSEWHERE, AND THE SENTENCE THAT USED TO SAY IT WAS UNBUILT. A paragraph here announced that "the
 * page's `cancel` algorithm is not yet invoked, and `tee`/`pipeTo`/`pipeThrough`/`from` and the BYTE stream are
 * absent". Every one of those five had since been built, and the sentence went on telling readers not to look:
 * the page's `cancel` is CALLED by the cancel machine below (step_call_run on the source's `cancel`, with
 * §4.9.4's PromiseCall semantics and ClearAlgorithms after it), `tee` and `from` are installed by this file,
 * `pipeTo` and `pipeThrough` are Streams §4.2.4 "Constructor, methods, and properties"' members over §4.9.1
 * "Working with readable streams"' ReadableStreamPipeTo and live in core/streams/pipe.c, and the byte stream is
 * core/streams/readable_byte_stream.c. THE DEFECT SHAPE IS WHY THIS IS WRITTEN DOWN RATHER THAN JUST DELETED: a
 * comment describing a mechanism as ABSENT is the one kind a reader does not verify, because every other kind
 * sends them to look and this one tells them it is not there. It cost a reading — a lane went to build
 * `textStream()` on Fetch §5.3 "Body mixin", read this line, and concluded the pipe machinery it needs was
 * unbuilt, when pipe.c is a complete step machine and the IDL gap audit reports ReadableStream, both readers and
 * both controllers as installing every member their IDL declares. So an absence claim in this component is
 * written only where the grep answers empty, and it names the grep that would refute it.
 *
 * THE QUEUE IS A JS ARRAY. Its chunks are the page's values and the collector must see them; an array is what
 * this component already has that the collector traces, and `gc_mark` on the class opaque is what makes the
 * stream itself a root for it. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/idl_async_iter.h"
#include "core/realm.h"
#include "core/streams/stream_work.h"
#include "solver/cow.h"
#include "core/structured_clone.h"
#include "core/streams/readable_stream.h"
#include "core/streams/readable_stream_impl.h"
#include "core/streams/readable_byte_stream.h"
#include "core/streams/pipe.h"

/* §4.2's states. A stream is readable until it is closed or errored, and those two are terminal. */
/* §4.2's three states — declared in the header, because pipeTo branches on them and two spellings of
   one enum is one more than a component may have. */

/* §4.2's stream and §4.4/§4.5's reader are declared in readable_stream_impl.h, because §4.7's controller
   performs §4.9.1's operations on the one and §4.5's reader IS the other. ONE record for both reader classes,
   because §4.3's mixin is most of both — see the record's own comment there. */

/* §4.6's controller. The three flags are the spec's own [[started]], [[pulling]] and [[pullAgain]], and they
   exist because `pull` is ASYNCHRONOUS: nothing may pull before start's promise fulfils, only one pull may be in
   flight, and a pull requested while one is in flight is remembered rather than dropped. */
typedef struct {
    JSValue stream;
    JSValue pull_fn;          /* the page's `pull`, or JS_UNDEFINED */
    JSValue cancel_fn;        /* the page's `cancel`, or JS_UNDEFINED */
    /* §4.9.4 builds each algorithm with CreateAlgorithmFromUnderlyingMethod, which INVOKES the method on the
       underlying source — so `start`, `pull` and `cancel` see it as their receiver, and a source written with
       methods that use `this` works. (The strategy's `size` is the exception: §7.4's ExtractSizeAlgorithm
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
/* THE AGENT'S POOL ENTRIES for the members whose declaration cannot be repeated per realm. */
static int       g_getreader_id = -1, g_tee_id = -1, g_tee_clone_id = -1;
static JSRuntime *g_rs_rt;
static int       g_ctor_stepid = -1, g_reader_ctor_stepid = -1, g_read_stepid = -1;
static int       g_byob_ctor_stepid = -1, g_from_ctor_stepid = -1;
static int       g_cancel_stepids[3] = { -1, -1, -1 };
static int       g_release_stepids[2] = { -1, -1 };
/* §4.5's PROTOTYPE, which is the one per-realm object of this component that is not a class proto: §4.4's and
   §4.5's readers are one CLASS (one record, one lock) and two interfaces, and quickjs's class-proto slot holds
   one object per class. */
static int       g_byob_proto_slot = -1;

static JSValue readable_byob_reader_proto(JSContext *ctx)   /* OWNED */
{
    return realm_value_get(ctx, g_byob_proto_slot);
}
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

/* ---- WHAT EACH RECORD OWNS, AND WHERE IT IS CAPTURED --------------------------------------------------------
 *
 * A stream's state is not in the page's heap — it is in these records, behind a class opaque no property hook
 * can see. So none of it time-travelled: one flow acquiring a reader held the lock for every sibling, and one
 * flow's read advanced the queue head for all of them. THE CAPTURE IS IN THE ACCESSOR, not at each write. A
 * record reached by a flow is a record that flow may write, the delta dedups it to one entry, and there are at
 * most a handful of streams in a flow — so capturing on REACH costs nothing measurable and removes the entire
 * class of "a write site was missed", which is the only way this can go wrong.
 * The finalizers and gc_marks go through JS_GetOpaque instead, deliberately: a capture during collection would
 * dup values on an object that is being torn down.
 * The offset lists say what the record OWNS, and they are the SAME lists the finalizers free. A field added to
 * one and not the other is exactly the bug this exists to prevent, which is why they are read together. */
#define RS_OFF(T, f) (uint16_t)offsetof(T, f)
#define RS_NVAL(a)   (int)(sizeof(a) / sizeof((a)[0]))
static const uint16_t STREAM_VALS[] = {
    RS_OFF(StreamData, stored_error), RS_OFF(StreamData, reader), RS_OFF(StreamData, queue),
    RS_OFF(StreamData, queue_size), RS_OFF(StreamData, read_resolve), RS_OFF(StreamData, read_reject),
    RS_OFF(StreamData, controller),
};
static const CowRecord STREAM_REC = { sizeof(StreamData), STREAM_VALS, RS_NVAL(STREAM_VALS) };

static const uint16_t READER_VALS[] = {
    RS_OFF(ReaderData, stream), RS_OFF(ReaderData, closed),
    RS_OFF(ReaderData, closed_funcs[0]), RS_OFF(ReaderData, closed_funcs[1]),
};
static const CowRecord READER_REC = { sizeof(ReaderData), READER_VALS, RS_NVAL(READER_VALS) };

static const uint16_t CTRL_VALS[] = {
    RS_OFF(ControllerData, stream), RS_OFF(ControllerData, pull_fn), RS_OFF(ControllerData, cancel_fn),
    RS_OFF(ControllerData, source), RS_OFF(ControllerData, size_fn),
};
static const CowRecord CTRL_REC = { sizeof(ControllerData), CTRL_VALS, RS_NVAL(CTRL_VALS) };

StreamData *rs_stream_data(JSValueConst v)
{
    StreamData *d = JS_GetOpaque(v, g_stream_class);
    if (d) cow_capture_host_record(v, d, &STREAM_REC);
    return d;
}
ReaderData *rs_reader_data(JSValueConst v)
{
    ReaderData *r = JS_GetOpaque(v, g_reader_class);
    if (r) cow_capture_host_record(v, r, &READER_REC);
    return r;
}
static ControllerData *ctrl_of(JSValueConst v)
{
    ControllerData *c = JS_GetOpaque(v, g_ctrl_class);
    if (c) cow_capture_host_record(v, c, &CTRL_REC);
    return c;
}

/* WRITE ONE OWNED SLOT OF EACH RECORD, and never `JS_FreeValue(ctx, d->f); d->f = <build one>;` — see cow.h for
   the order and the defect. §4's values are queues, promises and the page's own source methods: building one
   allocates, and an allocation IS a collection (js_trigger_gc has exactly one caller, JS_NewObjectFromShape),
   so a slot left naming freed storage across the build is walked by the record's own gc_mark and decrefs a
   JSObject already back on the allocator's free list. Releasing one is the same hazard from the other side.
   Each record binds its layout ONCE, here, so no site can pass a slot from one of the three with the layout of
   another. The MINTS do not come here: each fills its block before JS_SetOpaque, where the record is
   unreachable by the collector and its slots hold no value to release. */
void rs_stream_set_at(JSContext *ctx, StreamData *d, JSValue *slot, JSValue v,
                      const char *file, int line)
{
    cow_record_set_at(ctx, d, &STREAM_REC, slot, v, file, line);
}
/* THE ADDRESS PASSES THROUGH: the asserts inside are about the SLOT, so they must name the WRITE and not this
   line — see cow.h's THE SITE TRAVELS WITH THE OPERATION. */
static void rd_set_at(JSContext *ctx, ReaderData *r, JSValue *slot, JSValue v,
                      const char *file, int line)
{
    cow_record_set_at(ctx, r, &READER_REC, slot, v, file, line);
}
static void rc_set_at(JSContext *ctx, ControllerData *c, JSValue *slot, JSValue v,
                      const char *file, int line)
{
    cow_record_set_at(ctx, c, &CTRL_REC, slot, v, file, line);
}
#define rd_set(ctx_, r_, slot_, v_) rd_set_at((ctx_), (r_), (slot_), (v_), __FILE__, __LINE__)
#define rc_set(ctx_, c_, slot_, v_) rc_set_at((ctx_), (c_), (slot_), (v_), __FILE__, __LINE__)

/* How many chunks are still unread. §4.2 has no length to expose; this is the queue's own bookkeeping. */
static uint32_t stream_queued(JSContext *ctx, StreamData *d)
{
    JSValue len_v = JS_GetPropertyStr(ctx, d->queue, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, len_v);
    JS_FreeValue(ctx, len_v);
    return n - d->head;
}

/* §8.1's ResetQueue: the chunks, their sizes and the total go together, so they are dropped together. Every
   site that empties the queue calls this — an error, a cancel, a fresh stream — because a total left behind
   makes desiredSize answer for chunks that are gone. Returns -1 with an exception live. */
static int stream_queue_reset(JSContext *ctx, StreamData *d)
{
    JSValue q = JS_NewArray(ctx), qs = JS_NewArray(ctx);

    /* BUILT INTO LOCALS AND THEN PUBLISHED. Both Arrays are minted while the OLD two are still live and still
       named by the record, so neither of the two collections these allocations can start walks a slot naming
       freed storage — and an exception marker never reaches a slot stream_gc_mark reads. */
    if (JS_IsException(q) || JS_IsException(qs)) { JS_FreeValue(ctx, q); JS_FreeValue(ctx, qs); return -1; }
    rs_stream_set(ctx, d, &d->queue, q);
    rs_stream_set(ctx, d, &d->queue_size, qs);
    d->head = 0;
    d->queue_total = 0;
    return 0;
}

/* §8.1's EnqueueValueWithSize, minus its own RangeError — the caller checks that, because the check's failure
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

/* §8.1's DequeueValue: the chunk leaves and the total loses its size. The clamp at zero is the spec's own,
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

/* §4.9.2's ReadableStreamGetNumReadRequests / …GetNumReadIntoRequests. */
uint32_t rs_read_pending(JSContext *ctx, StreamData *d)
{
    JSValue len_v = JS_GetPropertyStr(ctx, d->read_resolve, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, len_v);
    JS_FreeValue(ctx, len_v);
    return n - d->rhead;
}

/* TAKE the next parked read request's resolve (or reject), or JS_UNDEFINED when none is parked. */
JSValue rs_take_read(JSContext *ctx, StreamData *d, int reject)
{
    JSValue f;
    if (rs_read_pending(ctx, d) == 0) return JS_UNDEFINED;
    f = JS_GetPropertyUint32(ctx, reject ? d->read_reject : d->read_resolve, d->rhead);
    /* the sibling capability of the pair is dropped with it: one read is answered once */
    JS_SetPropertyUint32(ctx, d->read_resolve, d->rhead, JS_UNDEFINED);
    JS_SetPropertyUint32(ctx, d->read_reject, d->rhead, JS_UNDEFINED);
    d->rhead++;
    return f;
}

/* §4.4's read() result: 7.4.14 CreateIterResultObject — `{ value, done }`. One per read request, because §4.2
   says so and because a shared object would be observable as identity.
   THE MEMBERS ARE DEFINED, NOT ASSIGNED. CreateIterResultObject uses CreateDataPropertyOrThrow, which is
   [[DefineOwnProperty]] and ignores the prototype chain entirely; an ASSIGNMENT is [[Set]], which consults it.
   The object is an ordinary one, so it inherits Object.prototype — and a page that has defined a getter-only
   `value` or `done` there would silently swallow the write and leave the read reaching its own getter. That is
   not a hypothetical: the same mistake on an internal-slot record is what made the engine abort inside its own
   event dispatch, and it looked nothing like a missing property. Anywhere this engine populates a fresh object
   it is creating own properties, which is DEFINE. */
JSValue rs_read_result(JSContext *ctx, JSValue value, bool done)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o)) { JS_FreeValue(ctx, value); return o; }
    JS_DefinePropertyValueStr(ctx, o, "value", value, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "done", JS_NewBool(ctx, done), JS_PROP_C_W_E);
    return o;
}

/* §4.5's controller, EMPTY and already attached — the same discipline readable_stream_empty follows, and for
   the reason its own comment gives: a record whose fields are placed before it is attached to its object is a
   record that leaks on any failure in between. */
static JSValue controller_new(JSContext *ctx, JSValueConst stream)
{
    ControllerData *c;
    JSValue obj;

    {
        JSValue proto = JS_GetClassProto(ctx, g_ctrl_class);
        DCHECK(!JS_IsNull(proto), "a §4.5 controller was minted in a realm that never ran its install");
        obj = JS_NewObjectProtoClass(ctx, proto, g_ctrl_class);
        JS_FreeValue(ctx, proto);
    }
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
    {
        JSValue proto = JS_GetClassProto(ctx, g_stream_class);
        DCHECK(!JS_IsNull(proto), "a ReadableStream was minted in a realm that never ran its install");
        obj = JS_NewObjectProtoClass(ctx, proto, g_stream_class);
        JS_FreeValue(ctx, proto);
    }
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
    d = rs_stream_data(obj);
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

/* Fetch §5.3's `textStream()` step 2 — see the header for why this is not the function above with no bytes. */
JSValue readable_stream_closed_empty(JSContext *ctx)
{
    StreamData *d;
    JSValue obj = readable_stream_empty(ctx);

    if (JS_IsException(obj)) return obj;
    d = rs_stream_data(obj);
    /* §4.9.2's ReadableStreamClose has three parts and only the FIRST of them has anything to act on here: it
       moves the state, then settles the reader's `closed` promise, then answers every parked read request. The
       other two are calls of the page's code, which is why closing a stream is a step machine everywhere else
       in this file — and why the ASSERT is the load-bearing line rather than the assignment. A stream that has
       just been minted cannot have been handed to a reader, so there is no promise to settle and no request to
       answer; the day a caller reaches this with a stream that HAS one, the two silent parts are the bug and
       this names it instead of quietly skipping them. */
    DCHECK(JS_IsUndefined(d->reader),
           "a stream reached §5.3 step 2's close already locked to a reader — §4.9.2's ReadableStreamClose "
           "must then settle that reader's `closed` promise and answer its parked read requests, and both of "
           "those are CALLS of the page's code that this synchronous entry cannot make. Close it through the "
           "cancel/close step machine instead");
    DCHECK(d->state == RS_READABLE,
           "a stream this function has just minted is already closed or errored — readable_stream_empty "
           "answers a stream in §4.2's `readable` state and nothing between there and here may move it");
    d->state = RS_CLOSED;
    /* The controller's own two flags, so a closed stream's controller reads the way §4.9.4's
       ReadableStreamDefaultControllerClose leaves one: close requested, and started because §9.2's "set up"
       ran no start algorithm for it to be waiting on. */
    ctrl_of(d->controller)->close_requested = 1;
    ctrl_of(d->controller)->started = 1;
    return obj;
}

bool readable_stream_is(JSValueConst v)
{
    return g_stream_class != 0 && JS_GetOpaque(v, g_stream_class) != NULL;
}

JSClassID readable_stream_class(void)
{
    return g_stream_class;
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
 * because exactly one call is ever in flight. The two enumerations are readable_stream_impl.h's, because §4.7's
 * controller runs the same two sequences. */

/* §4.9.2's ReadableStreamAddReadRequest / …AddReadIntoRequest. */
void rs_park_read(JSContext *ctx, StreamData *d, JSValue *funcs)
{
    uint32_t at = rs_read_pending(ctx, d) + d->rhead;

    JS_SetPropertyUint32(ctx, d->read_resolve, at, funcs[0]);
    JS_SetPropertyUint32(ctx, d->read_reject, at, funcs[1]);
}

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
            /* §4.9.3 Readers' ReadableStreamReaderGenericRelease step 5 — the OTHERWISE arm: a reader whose
               stream had ALREADY finished gets a NEW `closed` promise, already rejected (step 4 is the arm
               that rejects the one it has, for a stream still `readable`). The identity change is
               observable — `assert_not_equals` on the two promise objects is what the corpus checks — so it
               cannot be a no-op. */
            p = JS_NewPromiseCapability(ctx, funcs);
            if (JS_IsException(p)) { JS_FreeValue(ctx, in); return -1; }
            rd_set(ctx, rd, &rd->closed, p);
        } else {
            rd->closed_settled = 1;
            funcs[0] = rd->closed_funcs[0];               /* the pair is HANDED OVER and dropped together */
            funcs[1] = rd->closed_funcs[1];
            rd->closed_funcs[0] = rd->closed_funcs[1] = JS_UNDEFINED;
        }
        /* §4.9.2 Interfacing with controllers' ReadableStreamError step 7 and §4.9.3 Readers'
           ReadableStreamReaderGenericRelease step 6, which are the SAME sentence — "Set
           reader.[[closedPromise]].[[PromiseIsHandled]] to true" — and are therefore one condition and not two
           cases: the standard marks the arms that REJECT, and §4.9.2's ReadableStreamClose is the arm it does
           not because that one RESOLVES. `reject` already is that question.
           WITHOUT IT THE ENGINE REPORTED ITS OWN SPEC STEP AS THE PAGE'S UNHANDLED REJECTION. Nobody reads
           `closed` in the normal case — Fetch §2.2.4 "Bodies"'s fully-read releases the reader once a body is whole, and
           an errored stream usually has nothing awaiting it — so the TypeError §4.9.3's release rejects with reached
           solver/result.h's `pageErrors` as an uncaught page error on every run that finished reading a body,
           with a backtrace of one native frame and no script anywhere under it. A page error the ENGINE raised
           and no program of the document could have is the plausible-datum defect: it is indistinguishable in
           that column from a real one.
           IT IS SET BEFORE THE REJECTION, THOUGH THE STANDARD'S STEPS READ THE OTHER WAY. quickjs calls the
           host's rejection tracker FROM the reject, and it consults `is_handled` there, so a mark that lands
           after the reject has already been reported. [[PromiseIsHandled]] is not observable to the page, so
           the two orders differ in nothing else. */
        if (reject)
            JS_MarkPromiseHandled(ctx, rd->closed);
        JS_FreeValue(ctx, w->func);
        w->func = funcs[reject];
        JS_FreeValue(ctx, funcs[reject ^ 1]);
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

/* §4.9.2's ReadableStreamClose and ReadableStreamError and §4.9.3's ReadableStream*ReaderRelease, which are
 * ONE sequence with three entry points: move the stream's state (or, for a release, none), settle the
 * reader's `closed` promise, then answer EVERY parked read request. All three say "for each readRequest", and answering one is a CALL of the
 * page's code — so the tail is a LOOP OF CALL REQUESTS, one suspension per request. Answering only the first,
 * which is what this component did before the controller could pull, silently abandons the rest.
 * The caller sets `w->settle` (and `w->err` for the arms that carry a reason) and calls until it returns 0. */
int rs_settle_run(JSContext *ctx, StreamWork *w, StreamData *d, JSValue in,
                  JSValue **out_cb, int *out_argc)
{
    ReaderData *rd = JS_IsUndefined(d->reader) ? NULL : rs_reader_data(d->reader);
    int r;

    if (w->settle == S_CLOSE_SET) {
        if (d->state == RS_READABLE) d->state = RS_CLOSED;
        w->settle = S_CLOSE_CLOSED;
    } else if (w->settle == S_ERR_SET) {
        if (d->state == RS_READABLE) {
            d->state = RS_ERRORED;
            rs_stream_set(ctx, d, &d->stored_error, w->err);   /* HANDED OVER: the stream owns it from here */
            w->err = JS_UNDEFINED;
            /* §8.1's ResetQueue: an errored stream has no chunks left to give */
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
            /* §4.9.3 Readers' ReadableStreamReaderGenericRelease steps 8-9, in the one order that works: the
               `closed` promise is settled (steps 4-6) while the reader still holds the lock, and only then is
               the lock dropped. */
            DCHECK(rd != NULL, "a release sequence ran on a stream that has no reader");
            rs_stream_set(ctx, d, &d->reader, JS_UNDEFINED);
            rd_set(ctx, rd, &rd->stream, JS_UNDEFINED);
            /* …AND THE SECOND TYPEERROR, WHICH IS A DIFFERENT OBJECT AND NOT A SECOND SPELLING OF THE FIRST.
               §4.9.3's ReadableStreamDefaultReaderRelease is three steps — "Perform !
               ReadableStreamReaderGenericRelease(reader)", then "Let e be a NEW TypeError exception", then
               "Perform ! ReadableStreamDefaultReaderErrorReadRequests(reader, e)" — and
               ReadableStreamBYOBReaderRelease is the same three over the read-INTO requests. GenericRelease
               step 4/5 already rejected `closed` with a TypeError of its own, so a release that reuses that
               one object hands the page a `read()` rejection and a `closed` rejection that compare `===`,
               which no browser does and which a page can see with two `.catch`es and one comparison.
               THE MESSAGE IS THE SAME BECAUSE THE STANDARD DEFINES NEITHER — what it defines is that there
               are two exceptions, and identity is the only part of that a page can observe.
               `w->err` HELD THE FIRST AND IS SAFE TO DROP HERE: reader_closed_run above already handed its
               own reference to the reject (`w->value` holds the dup that the settling call consumes), and the
               S_REL_LOOP tail below re-dups out of `w->err` for every parked request. */
            JS_FreeValue(ctx, w->err);
            JS_ThrowTypeError(ctx, "this reader was released while it was still in use");
            w->err = JS_GetException(ctx);
            w->settle = S_REL_LOOP;
        } else {
            w->settle = w->settle == S_CLOSE_CLOSED ? S_CLOSE_LOOP : S_ERR_LOOP;
        }
    }

    DCHECK(w->settle == S_CLOSE_LOOP || w->settle == S_ERR_LOOP || w->settle == S_REL_LOOP ||
           w->settle == S_INTO_LOOP, "the settle sequence resumed in a state it never parks in");

    /* §4.9.2's ReadableStreamClose answers a DEFAULT reader's read requests and says nothing about a BYOB
       reader's read-into requests — they stay parked, because the memory they were given has not come back
       yet. Only a CANCEL returns them (with the memory deliberately dropped), and that is S_INTO_LOOP. */
    if (w->settle == S_CLOSE_LOOP && rd && rd->byob) {
        JS_FreeValue(ctx, in);
        JS_FreeValue(ctx, w->err);
        w->err = JS_UNDEFINED;
        w->settle = S_IDLE;
        return 0;
    }

    for (;;) {
        JSValueConst arg;
        JSValue out;

        if (w->phase == 0) {
            int reject = w->settle == S_ERR_LOOP || w->settle == S_REL_LOOP;
            JS_FreeValue(ctx, w->func);
            w->func = rs_take_read(ctx, d, reject);
            if (JS_IsUndefined(w->func)) {
                JS_FreeValue(ctx, in);
                JS_FreeValue(ctx, w->err);
                w->err = JS_UNDEFINED;
                w->settle = S_IDLE;
                return 0;
            }
            JS_FreeValue(ctx, w->value);
            /* A CLOSE and a CANCEL both answer `{ value: undefined, done: true }` — the cancel deliberately
               DROPS the backing memory a BYOB read supplied, which is what §4.5 says its close steps do when
               they are given undefined rather than a view. */
            w->value = w->settle == S_ERR_LOOP ? JS_DupValue(ctx, d->stored_error)
                     : w->settle == S_REL_LOOP ? JS_DupValue(ctx, w->err)
                                               : rs_read_result(ctx, JS_UNDEFINED, true);
            if (JS_IsException(w->value)) { JS_FreeValue(ctx, in); return -1; }
        }
        arg = w->value;
        r = step_call_run(ctx, &w->phase, STEP_CB(w->cb), w->func, JS_UNDEFINED, 1, &arg, in, &out, out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return -1;
        JS_FreeValue(ctx, out);
        in = JS_UNDEFINED;
    }
}

/* §4.5's reactions, by what they react to. Each is a step closure capturing the controller. */
enum { RXN_START_OK = 0, RXN_START_ERR, RXN_PULL_OK, RXN_PULL_ERR };

/* WHERE THIS MACHINE RESTS. Each of the four is a REACTION the spec attaches at a named step, and what it does
   afterwards — pull again, or error the stream — is the other stage. */
#define RXN_STAGES(X) \
    X(RXN_DECIDE, "Streams §4.9.4 SetUpReadableStreamDefaultController steps 11-12 / §4.5 " \
                  "ReadableStreamDefaultControllerCallPullIfNeeded steps 6-7 (which reaction this is: the " \
                  "controller is started, a pull may repeat, or an error is to be reported)") \
    X(RXN_RUN, "Streams §4.9.4 ReadableStreamDefaultControllerCallPullIfNeeded steps 2-7 / §4.2 " \
               "ReadableStreamError (the pull the reaction asks for, or the error it reports)")
enum { RXN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RXN_STEPS[] = { RXN_STAGES(JS_STEP_STAGE_LABEL) NULL };

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

static int js_fwd_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSFwdState *s = st;
    JSValueConst arg = s->hdr.arg == FWD_ARG && s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED;
    JSValue out;
    int r;

    r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), JS_StepClosureData(&s->hdr, 0), JS_UNDEFINED, 1, &arg,
                      cb_result, &out, out_cb, out_argc);
    if (r > 0) return r;
    if (JS_IsException(out)) return JS_STEP_ABRUPT;
    JS_FreeValue(ctx, out);
    return JS_STEP_DONE;
}

/* ONE STAGE, because the whole of this machine is the call — it holds nothing between entries. */
#define FWD_STAGES(X) \
    X(FWD_CALL, "Streams §4 (a reaction forwarding a promise's settlement to a resolving function — that " \
                "function's 27.5.1.3 step 2.f `then` read is the page's code)")
enum { FWD_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FWD_STEPS[] = { FWD_STAGES(JS_STEP_STAGE_LABEL) NULL };

#define FWD_DEF(i) { sizeof(JSFwdState), js_fwd_step, NULL, (i), \
                     .catches_abrupt = 1, .visit = js_fwd_visit, \
                     .algorithm = "Streams §4 (forward a promise's settlement to a resolving function)", \
                     .steps = FWD_STEPS }
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

/* §4.9.4's ReadableStreamDefaultControllerCanCloseOrEnqueue. */
static bool ctrl_can_close_or_enqueue(ControllerData *c, StreamData *d)
{
    return !c->close_requested && d->state == RS_READABLE;
}

/* §4.9.4's ReadableStreamDefaultControllerShouldCallPull. The default strategy's high-water mark is 1, so the
   desired size is 1 minus what is queued — which is why a source with an empty queue is pulled once even
   though nobody is reading, and why a source with a chunk in hand is not. */
static bool ctrl_should_pull(JSContext *ctx, ControllerData *c, StreamData *d)
{
    if (!ctrl_can_close_or_enqueue(c, d)) return false;
    if (!c->started) return false;
    if (!JS_IsUndefined(d->reader) && rs_read_pending(ctx, d) > 0) return true;
    return c->hwm - d->queue_total > 0;
}

/* §4.9.4's ReadableStreamDefaultControllerCallPullIfNeeded. The caller sets `w->pull = P_TEST` and calls until
   it returns 0; every machine that can change what ShouldCallPull answers runs it.
   A stream has ONE controller and it is §4.6's or §4.7's, so this is where the shared machines — the read, the
   cancel — ask WHICH of the two CallPullIfNeeded operations §4.9.2 means. A value that is neither aborts inside
   §4.9.5's, which asserts its own receiver. */
static int ctrl_pull_run(JSContext *ctx, StreamWork *w, JSValueConst ctrl_v, JSValue in,
                         JSValue **out_cb, int *out_argc)
{
    ControllerData *c = ctrl_of(ctrl_v);
    StreamData *d;
    int r;

    if (!c) return readable_byte_pull_run(ctx, w, ctrl_v, in, out_cb, out_argc);
    d = rs_stream_data(c->stream);
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
        r = step_call_run(ctx, &w->phase, STEP_CB(w->cb), c->pull_fn, c->source, 1, &arg, in, &res,
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

static int js_rxn_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSRxnState *s = st;
    JSValueConst ctrl = JS_StepClosureData(&s->hdr, 0);
    ControllerData *c = ctrl_of(ctrl);
    StreamData *d;
    int r;

    DCHECK(c != NULL, "a stream reaction captured something that is not a ReadableStreamDefaultController");
    d = rs_stream_data(c->stream);
    DCHECK(d != NULL, "a controller's stream stopped being a ReadableStream");

    if (s->hdr.stage == RXN_DECIDE) {
        int op = s->hdr.arg;
        stream_work_start(&s->w);
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->hdr.stage = RXN_RUN;
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
                            : rs_settle_run(ctx, &s->w, d, cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    return JS_STEP_DONE;
}

#define RXN_DEF(i, alg) { sizeof(JSRxnState), js_rxn_step, NULL, (i), \
                          .catches_abrupt = 1, .visit = js_rxn_visit, \
                          .algorithm = (alg), .steps = RXN_STEPS }
static const JSTrampStepDef js_rxn_defs[4] = {
    RXN_DEF(RXN_START_OK,
            "Streams \u00a74.9.4 SetUpReadableStreamDefaultController step 11's onFulfilled"),
    RXN_DEF(RXN_START_ERR,
            "Streams \u00a74.9.4 SetUpReadableStreamDefaultController step 12's onRejected"),
    RXN_DEF(RXN_PULL_OK,
            "Streams \u00a74.5 ReadableStreamDefaultControllerCallPullIfNeeded step 6's onFulfilled"),
    RXN_DEF(RXN_PULL_ERR,
            "Streams \u00a74.5 ReadableStreamDefaultControllerCallPullIfNeeded step 7's onRejected"),
};
#undef RXN_DEF

/* ---- §4.4's reader ---------------------------------------------------------------------------------------- */

/* §4.4's `read()`. A MACHINE, because settling the promise it returns is the PAGE'S code: 27.5.1.3 step 2.f
 * reads `Get(resolution, "then")` off the result object, whose prototype the page owns. The same reason
 * body.c's readers are machines, and the same shape.
 *
 * It also performs §4.5's PullSteps, which is where a stream's `pull` is asked for on demand — and the spec's
 * ORDER is that the close-or-pull happens BEFORE the read request is answered, so those are stages ahead of the
 * settle rather than after it. */
/* WHERE THIS MACHINE RESTS. §4.4's read() is three steps over §4.9.3's ReadableStreamDefaultReaderRead, whose
   step 6.2 is §4.5's [[PullSteps]] — and the close-or-pull that operation performs runs BEFORE the read
   request is answered, which is why those are stages ahead of the settle rather than after it. */
#define RD_STAGES(X) \
    X(RD_START, "Streams §4.4 read() steps 1-3 and §4.9.3 ReadableStreamDefaultReaderRead steps 1-6 (the brand, " \
                "the released refusal, the promise, and what the stream's state answers with)") \
    X(RD_CLOSE, "Streams §4.5 [[PullSteps]] step 3.3 (draining the last chunk of a stream whose close was " \
                "requested performs ReadableStreamClose)") \
    X(RD_PULL, "Streams §4.5 [[PullSteps]] step 3.4 / step 4 (CallPullIfNeeded, because the drain made room " \
               "or because the request parked)") \
    X(RD_SETTLE, "Streams §4.9.3 ReadableStreamDefaultReaderRead steps 4-6 (the read request's close, error or " \
                 "chunk steps — settling the promise is the page's code)")
enum { RD_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RD_STEPS[] = { RD_STAGES(JS_STEP_STAGE_LABEL) NULL };

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
    (void)ctx;
    if (take_result) s->promise = JS_UNDEFINED;
    return r;
}

static int js_read_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSReadState *s = st;
    JSValue out;
    JSValueConst arg;
    int r;

    if (s->hdr.stage == RD_START) {
        ReaderData *rd = rs_reader_data(s->hdr.this_val);
        StreamData *d = NULL;
        JSValue funcs[2];
        int reject = 0;

        stream_work_start(&s->w);
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->promise = s->settle = s->result = s->stream = JS_UNDEFINED;
        if (!rd || rd->byob) {
            /* §4.4's `read()` is the DEFAULT reader's; a BYOB reader implements §4.5's, which takes a view. */
            JS_ThrowTypeError(ctx, "not a ReadableStreamDefaultReader");
            return JS_STEP_ABRUPT;
        }
        STEP_GOTO(s->hdr.stage, RD_SETTLE, &s->w.phase, NULL);
        if (JS_IsUndefined(rd->stream)) {
            /* §4.4: a released reader's read() REJECTS rather than throwing — it is a promise-returning
               member, and a page's `.catch` is where it expects to see this. */
            JS_ThrowTypeError(ctx, "this reader has been released");
            s->result = JS_GetException(ctx);
            reject = 1;
        } else {
            d = rs_stream_data(rd->stream);
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
                s->result = rs_read_result(ctx, JS_UNDEFINED, true);
            } else if (readable_byte_ctrl_is(d->controller)
                       ? readable_byte_has_chunk(d->controller) : stream_queued(ctx, d) > 0) {
                /* §4.9.2's [[PullSteps]], and the ONE thing the two controllers disagree about here: §4.6
                   dequeues the chunk the page enqueued, §4.7 takes the head BYTE RANGE off its queue and hands
                   back a Uint8Array over exactly those bytes. What follows — the close-or-pull, then the
                   answer — is the same operation for both. */
                bool bytes = readable_byte_ctrl_is(d->controller);
                JSValue chunk = bytes ? readable_byte_take_chunk(ctx, d->controller) : stream_dequeue(ctx, d);
                bool drained;
                if (JS_IsException(chunk)) return JS_STEP_ABRUPT;
                s->result = rs_read_result(ctx, chunk, false);
                if (bytes) {
                    drained = readable_byte_drained(d->controller);
                    if (drained) readable_byte_clear_algorithms(ctx, d->controller);
                } else {
                    ControllerData *c = ctrl_of(d->controller);
                    DCHECK(c != NULL, "a ReadableStream reached a read with no controller");
                    drained = c->close_requested && stream_queued(ctx, d) == 0;
                }
                /* HandleQueueDrain: draining the LAST chunk of a stream whose close was requested is what
                   actually closes it — a page that calls close() with chunks still queued must see every one
                   of them before `done`. Otherwise the drain is what makes room, so the source is pulled. */
                if (drained) {
                    STEP_GOTO(s->hdr.stage, RD_CLOSE, &s->w.phase, NULL);
                    s->w.settle = S_CLOSE_SET;
                } else {
                    STEP_GOTO(s->hdr.stage, RD_PULL, &s->w.phase, NULL);
                    s->w.pull = P_TEST;
                }
            } else {
                /* §4.2: a READABLE stream with an empty queue PARKS the read request. The promise is returned
                   unsettled and the controller answers it later — which is the whole reason a stream is not
                   just a queue. The source is then pulled, because a parked reader is what ShouldCallPull is
                   most interested in. */
                if (readable_byte_ctrl_is(d->controller)) {
                    /* §4.7's [[PullSteps]] steps 4-5: a byte source that declared an autoAllocateChunkSize
                       gets a buffer of its own to fill, BEFORE the request is parked. A buffer that could not
                       be constructed is the read request's ERROR steps, not a throw. */
                    JSValue alloc = readable_byte_auto_allocate(ctx, d->controller);
                    if (JS_IsException(alloc)) {
                        s->result = JS_GetException(ctx);
                        reject = 1;
                        goto have_result;
                    }
                }
                s->promise = JS_NewPromiseCapability(ctx, funcs);
                if (JS_IsException(s->promise)) return JS_STEP_ABRUPT;
                rs_park_read(ctx, d, funcs);
                STEP_GOTO(s->hdr.stage, RD_PULL, &s->w.phase, NULL);
                s->w.pull = P_TEST;
                goto have_promise;
            }
        have_result:
            if (JS_IsException(s->result)) return JS_STEP_ABRUPT;
        }
        s->promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(s->promise)) return JS_STEP_ABRUPT;
        s->settle = funcs[reject];
        JS_FreeValue(ctx, funcs[reject ^ 1]);
    have_promise:;
    }

    if (s->hdr.stage == RD_CLOSE) {
        r = rs_settle_run(ctx, &s->w, rs_stream_data(s->stream), cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        STEP_GOTO(s->hdr.stage, RD_SETTLE, &s->w.phase, NULL);
    }
    if (s->hdr.stage == RD_PULL) {
        StreamData *d = rs_stream_data(s->stream);
        DCHECK(d != NULL, "the read machine's stream stopped being a ReadableStream");
        r = ctrl_pull_run(ctx, &s->w, d->controller, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        STEP_GOTO(s->hdr.stage, RD_SETTLE, &s->w.phase, NULL);
    }

    DCHECK(s->hdr.stage == RD_SETTLE, "the read machine resumed in a stage it never parks in");
    if (JS_IsUndefined(s->settle)) {
        /* the request parked: nothing to settle yet, the controller owns the answer */
        JS_FreeValue(ctx, cb_result);
        return JS_STEP_DONE;
    }
    arg = s->result;
    r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->settle, JS_UNDEFINED, 1, &arg, cb_result, &out,
                      out_cb, out_argc);
    if (r > 0) return r;
    if (JS_IsException(out)) return JS_STEP_ABRUPT;
    JS_FreeValue(ctx, out);
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_read_def = {
    sizeof(JSReadState), js_read_step, js_read_fini, 0, .catches_abrupt = 1, .visit = js_read_visit,
    .algorithm = "Streams §4.4 read(), through §4.9.3 ReadableStreamDefaultReaderRead", .steps = RD_STEPS
};

/* §4.4's and §4.5's `releaseLock()`. A MACHINE, because releasing is not just dropping the lock: it REJECTS
 * the reader's `closed` promise with a TypeError and rejects every read request the reader had parked — both of which settle
 * promises, which is the page's code. A release that only dropped the lock left a page awaiting `reader.closed`
 * forever, and a `read()` issued before it never answered at all; that is what made the corpus's whole
 * default-reader file report NOTHING rather than a failure, because testharness cannot complete while a
 * promise_test is still unsettled. */
/* WHERE THIS MACHINE RESTS. §4.4's and §4.5's releaseLock() are one step over §4.9.3's
   ReadableStreamDefaultReaderRelease and ReadableStreamBYOBReaderRelease, which differ in NOTHING this
   component can see — both are GenericRelease and then a TypeError into every request the reader had parked,
   and both lists are the same two arrays. The magic is the BRAND, which is not the same thing: a default
   reader handed to §4.5's releaseLock is a TypeError. */
#define REL_STAGES(X) \
    X(REL_ENTRY, "Streams §4.4/§4.5 releaseLock() steps 1-2 and §4.9.3 ReadableStream*ReaderRelease steps 1-3 " \
                 "(the brand, the already-released no-op, the controller's [[ReleaseSteps]], and the released " \
                 "TypeError)") \
    X(REL_SETTLE, "Streams §4.9.3 ReadableStream*ReaderRelease steps 4-5 (reject the reader's `closed` " \
                  "and every request it had parked)")
enum { REL_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const REL_STEPS[] = { REL_STAGES(JS_STEP_STAGE_LABEL) NULL };

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

static int js_release_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSReleaseState *s = st;
    StreamData *d;
    int r;

    if (s->hdr.stage == REL_ENTRY) {
        ReaderData *rd = rs_reader_data(s->hdr.this_val);
        stream_work_start(&s->w);
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->stream = JS_UNDEFINED;
        s->hdr.stage = REL_SETTLE;
        if (!rd || rd->byob != (s->hdr.arg != 0)) {
            JS_ThrowTypeError(ctx, s->hdr.arg ? "not a ReadableStreamBYOBReader"
                                              : "not a ReadableStreamDefaultReader");
            return JS_STEP_ABRUPT;
        }
        if (JS_IsUndefined(rd->stream)) return JS_STEP_DONE;   /* §4.4/§4.5: releasing twice is a no-op */
        s->stream = JS_DupValue(ctx, rd->stream);
        /* §4.9.2's [[ReleaseSteps]], which is the one part of a release that is the CONTROLLER's: §4.7 keeps
           the pull-into this reader was being filled into, marked as belonging to no reader. */
        {
            StreamData *sd = rs_stream_data(s->stream);
            DCHECK(sd != NULL, "a reader held something that is not a ReadableStream");
            if (readable_byte_ctrl_is(sd->controller)) readable_byte_release_steps(ctx, sd->controller);
        }
        JS_ThrowTypeError(ctx, "this reader was released while it was still in use");
        s->w.err = JS_GetException(ctx);
        s->w.settle = S_REL_CLOSED;
    }
    if (s->w.settle == S_IDLE) { JS_FreeValue(ctx, cb_result); return JS_STEP_DONE; }
    d = rs_stream_data(s->stream);
    DCHECK(d != NULL, "a reader held something that is not a ReadableStream");
    r = rs_settle_run(ctx, &s->w, d, cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    return JS_STEP_DONE;
}

#define REL_DEF(i) { sizeof(JSReleaseState), js_release_step, NULL, (i), .catches_abrupt = 1, \
                     .visit = js_release_visit, \
                     .algorithm = "Streams §4.4/§4.5 releaseLock(), through §4.9.3 ReadableStream*ReaderRelease", \
                     .steps = REL_STEPS }
static const JSTrampStepDef js_release_defs[2] = { REL_DEF(0), REL_DEF(1) };
#undef REL_DEF

/* §4.3's `closed`, the mixin BOTH reader interfaces include — and the magic is which of the two prototypes
   this copy of it was installed on, because a mixin's member still brand-checks against the interface that
   includes it. */
static JSValue js_reader_closed(JSContext *ctx, JSValueConst this_val, int magic)
{
    ReaderData *r = rs_reader_data(this_val);
    if (!r || r->byob != (magic != 0))
        return JS_ThrowTypeError(ctx, magic ? "not a ReadableStreamBYOBReader"
                                            : "not a ReadableStreamDefaultReader");
    return JS_DupValue(ctx, r->closed);
}

/* ---- §4.2's members --------------------------------------------------------------------------------------- */

static JSValue js_stream_locked(JSContext *ctx, JSValueConst this_val, int magic)
{
    StreamData *d = rs_stream_data(this_val);
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

/* §4.2's `getReader(options)` and §4.4's and §4.5's constructors, which ARE one operation — §4.9.3's
 * SetUpReadableStreamDefaultReader and SetUpReadableStreamBYOBReader differ by ONE step (the byte-controller
 * check) and share ReadableStreamReaderGenericInitialize — so they are one machine reached with three magics.
 *
 * A MACHINE because §4.9.3's GenericInitialize settles `closed` IMMEDIATELY when the stream is already closed or
 * errored: a reader taken on a finished stream must find its `closed` promise already settled, and settling is
 * the page's code. `mode: "byob"` names §4.5's reader, which is the same record and the same lock over a
 * different prototype — and a TypeError when the stream's controller is not §4.7's. */
enum { GR_SELF = 0, GR_CTOR, GR_CTOR_BYOB };   /* the magic: which argument the stream arrives in, and which
                                                  interface is being constructed */

/* WHERE THIS MACHINE RESTS. §4.9.3's SetUpReadableStream*Reader is four steps, and only the last of them —
   GenericInitialize settling `closed` for a stream that has already finished — is the page's code. */
#define GR_STAGES(X) \
    X(GRS_SETUP = IDL_STEP_FIRST, \
      "Streams §4.9.3 SetUpReadableStreamDefaultReader / SetUpReadableStreamBYOBReader steps 1-3 and " \
      "ReadableStreamReaderGenericInitialize steps 1-3 (the lock, the reader, and its `closed` capability)") \
    X(GRS_DONE, "Streams §4.2 getReader(options) steps 1-3 (the reader AcquireReadableStream*Reader " \
                "answered, for a stream still readable)") \
    X(GRS_SETTLE, "Streams §4.9.3 ReadableStreamReaderGenericInitialize step 2/3 (settling `closed` at once, " \
                  "because the stream was already closed or errored)")
enum { GR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const GR_STEPS[] = { GR_STAGES(JS_STEP_STAGE_LABEL) NULL };

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

static int js_get_reader_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                              JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSGetReaderState *s = st;
    int magic = idl_step_magic(hdr);
    int ctor = magic != GR_SELF;
    JSValueConst stream_v = ctor ? (argc > 0 ? argv[0] : JS_UNDEFINED) : hdr->this_val;
    JSValueConst arg;
    JSValue out;
    int byob = magic == GR_CTOR_BYOB;
    int r;

    if (hdr->stage == GRS_SETUP) {
        StreamData *d = rs_stream_data(stream_v);
        ReaderData *rd;
        JSValue obj;

        stream_work_start(&s->w);
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->reader = JS_UNDEFINED;
        STEP_GOTO(hdr->stage, GRS_DONE, &s->w.phase, NULL);
        if (ctor && JS_IsUndefined(hdr->this_val)) {
            JS_ThrowTypeError(ctx, "constructor %s requires 'new'",
                              byob ? "ReadableStreamBYOBReader" : "ReadableStreamDefaultReader");
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
            byob = !JS_IsUndefined(mode);
            JS_FreeValue(ctx, mode);
        }
        /* §4.9.3's SetUpReadableStreamBYOBReader step 2: a BYOB reader exists only over §4.7's controller,
           because it is the only one that can be given memory to fill. */
        if (byob && !readable_byte_ctrl_is(d->controller)) {
            JS_ThrowTypeError(ctx, "this stream is not a byte stream, so it has no BYOB reader");
            return -1;
        }
        {
            JSValue proto = byob ? readable_byob_reader_proto(ctx) : JS_GetClassProto(ctx, g_reader_class);
            DCHECK(!JS_IsNull(proto) && !JS_IsUndefined(proto),
                   "a reader was minted in a realm that never ran its install");
            obj = JS_NewObjectProtoClass(ctx, proto, g_reader_class);
            JS_FreeValue(ctx, proto);
        }
        if (JS_IsException(obj)) return -1;
        rd = js_mallocz(ctx, sizeof *rd);
        if (!rd) { JS_FreeValue(ctx, obj); return -1; }
        rd->byob = (uint8_t)byob;
        rd->stream = JS_DupValue(ctx, stream_v);
        rd->closed = JS_NewPromiseCapability(ctx, rd->closed_funcs);
        JS_SetOpaque(obj, rd);
        s->reader = obj;
        if (JS_IsException(rd->closed)) return -1;
        d->reader = JS_DupValue(ctx, obj);
        /* §4.9.3's ReadableStreamReaderGenericInitialize: a stream that is ALREADY finished settles `closed`
           here and now. */
        if (d->state == RS_READABLE) goto done;
        rd->closed_settled = 1;
        s->w.func = rd->closed_funcs[d->state == RS_ERRORED];
        JS_FreeValue(ctx, rd->closed_funcs[d->state == RS_ERRORED ? 0 : 1]);
        rd->closed_funcs[0] = rd->closed_funcs[1] = JS_UNDEFINED;
        /* §4.9.3 Readers' ReadableStreamReaderGenericInitialize step 5.3 — the THIRD arm only. Steps 3 and 4
           mint a pending `closed` and one resolved with undefined, and neither is marked; step 5's is "a
           promise rejected with stream.[[storedError]]", and the sentence after it is "Set
           reader.[[closedPromise]].[[PromiseIsHandled]] to true". So this is the same one condition the
           settle path asks — the arm that REJECTS — reached here because acquiring a reader over a stream that
           has ALREADY errored rejects `closed` in the set-up rather than through §4.9.2's error.
           Without it, `new ReadableStreamDefaultReader(erroredStream)` reported the stream's own stored error
           as the page's unhandled rejection, which is a page error no program of the document raised.
           BEFORE the rejection, for the reason stated at reader_closed_run's mark. */
        if (d->state == RS_ERRORED) JS_MarkPromiseHandled(ctx, rd->closed);
        s->w.value = d->state == RS_ERRORED ? JS_DupValue(ctx, d->stored_error) : JS_UNDEFINED;
        STEP_GOTO(hdr->stage, GRS_SETTLE, &s->w.phase, NULL);
    }

    if (hdr->stage == GRS_SETTLE) {
        arg = s->w.value;
        r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->w.func, JS_UNDEFINED, 1, &arg, cb_result, &out,
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
    js_get_reader_step, sizeof(JSGetReaderState), js_get_reader_visit, NULL,
    "Streams §4.7 SetUpReadableStreamDefaultReader (getReader() and the reader constructor both)", GR_STEPS
};

/* §4.9.2's ReadableStreamCancel, reached from the stream's `cancel(reason)` and from the reader's — §4.3 says
 * a reader cancels the stream it holds, so the two ARE one operation and one machine.
 *
 * IT CALLS THE SOURCE'S OWN `cancel`, and the promise it hands back settles when the SOURCE'S does: §4.2 step 7
 * is "the result of reacting to sourceCancelPromise with a fulfilment step that returns undefined". That
 * reaction is a step closure over this component's own resolving function, the same shape §4.5's pull
 * reactions are. Before this, the page's `cancel` was never invoked at all — a stream whose source releases a
 * socket released nothing, and the returned promise settled ahead of the source rather than with it. */
/* WHERE THIS MACHINE RESTS, AS §4.2 NUMBERS IT. ReadableStreamCancel is seven steps and three of them are the
   page's code: closing the stream settles `closed`, the source's own `cancel` is a call, and the reaction to
   what it answered is a PromiseResolve. */
/* WHICH INTERFACE this copy of `cancel` was installed on — §4.2's, §4.4's or §4.5's. A mixin's member still
   brand-checks against the interface that includes it, so the three are three declarations. */
enum { CANCEL_ON_STREAM = 0, CANCEL_ON_DEFAULT, CANCEL_ON_BYOB };

#define CN_STAGES(X) \
    X(CN_START, "Streams §4.9.2 ReadableStreamCancel steps 1-3 (disturbed, then the closed/errored " \
                "short-circuits)") \
    X(CN_CLOSE, "Streams §4.9.2 ReadableStreamCancel step 4's ReadableStreamClose (the reader's `closed` " \
                "resolves and its parked read requests are answered)") \
    X(CN_CLOSE_INTO, "Streams §4.9.2 ReadableStreamCancel step 6 (a BYOB reader's read-into requests are " \
                     "answered with undefined — the backing memory they lent is discarded)") \
    X(CN_CALL, "Streams §4.9.2 ReadableStreamCancel step 7 (the controller's [[CancelSteps]] — the source's " \
               "own `cancel` algorithm over the reason)") \
    X(CN_RESOLVE, "Streams §4.9.2 ReadableStreamCancel step 8 (PromiseResolve over what the source answered)") \
    X(CN_THEN, "Streams §4.9.2 ReadableStreamCancel step 8 (reacting to sourceCancelPromise with a fulfilment " \
               "step that returns undefined)") \
    X(CN_SETTLE, "Streams §4.2 cancel(reason) steps 1-2 (settling this member's own answer — a rejected " \
                 "promise for a locked stream, or undefined)")
enum { CN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CN_STEPS[] = { CN_STAGES(JS_STEP_STAGE_LABEL) NULL };

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
    (void)ctx;
    if (take_result) s->promise = JS_UNDEFINED;
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

    if (s->hdr.stage == CN_START) {
        /* The receiver is the STREAM for `stream.cancel()` and the READER for `reader.cancel()` — and WHICH
           reader interface is the magic, because §4.3's mixin is included by two of them and a member of one
           interface's prototype brand-checks against that interface. */
        ReaderData *rd;
        int magic = s->hdr.arg;
        int reject = 0;

        stream_work_start(&s->w);
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->promise = s->funcs[0] = s->funcs[1] = s->settle = s->stream = JS_UNDEFINED;
        d = magic == CANCEL_ON_STREAM ? rs_stream_data(s->hdr.this_val) : NULL;
        rd = magic == CANCEL_ON_STREAM ? NULL : rs_reader_data(s->hdr.this_val);
        if (rd && rd->byob != (magic == CANCEL_ON_BYOB)) rd = NULL;
        if (rd && !JS_IsUndefined(rd->stream)) d = rs_stream_data(rd->stream);
        s->promise = JS_NewPromiseCapability(ctx, s->funcs);
        if (JS_IsException(s->promise)) return JS_STEP_ABRUPT;
        STEP_GOTO(s->hdr.stage, CN_SETTLE, &s->w.phase, NULL);
        if (!d) {
            JS_ThrowTypeError(ctx, magic == CANCEL_ON_STREAM ? "not a ReadableStream"
                                 : rd                        ? "this reader has been released"
                                 : magic == CANCEL_ON_BYOB   ? "not a ReadableStreamBYOBReader"
                                                             : "not a ReadableStreamDefaultReader");
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
                STEP_GOTO(s->hdr.stage, CN_CLOSE, &s->w.phase, NULL);
                s->w.settle = S_CLOSE_SET;
            }
        }
        if (s->hdr.stage == CN_SETTLE) {
            s->settle = s->funcs[reject];
            s->funcs[reject] = JS_UNDEFINED;
        }
    }

    if (s->hdr.stage == CN_CLOSE) {
        /* §4.9.1 step 4: the stream CLOSES before the source is asked to cancel, so a reader awaiting `closed`
           or parked on a `read()` is answered first. */
        r = rs_settle_run(ctx, &s->w, rs_stream_data(s->stream), cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        STEP_GOTO(s->hdr.stage, CN_CLOSE_INTO, &s->w.phase, NULL);
        {
            /* §4.9.1 step 6, which ReadableStreamClose deliberately does NOT do: a BYOB reader's read-into
               requests are answered here, with `undefined` rather than a view — a cancel DISCARDS the backing
               memory the reader lent, and that difference is page-visible. */
            StreamData *sd = rs_stream_data(s->stream);
            ReaderData *rd = sd && !JS_IsUndefined(sd->reader) ? rs_reader_data(sd->reader) : NULL;
            if (rd && rd->byob) s->w.settle = S_INTO_LOOP;
        }
    }

    if (s->hdr.stage == CN_CLOSE_INTO) {
        if (s->w.settle != S_IDLE) {
            r = rs_settle_run(ctx, &s->w, rs_stream_data(s->stream), cb_result, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            cb_result = JS_UNDEFINED;
        }
        STEP_GOTO(s->hdr.stage, CN_CALL, &s->w.phase, NULL);
    }

    if (s->hdr.stage == CN_CALL) {
        JSValueConst cancel_fn = JS_UNDEFINED, source = JS_UNDEFINED;
        int bytes;

        d = rs_stream_data(s->stream);
        DCHECK(d != NULL, "the cancel machine's stream stopped being a ReadableStream");
        bytes = readable_byte_ctrl_is(d->controller);
        c = bytes || JS_IsUndefined(d->controller) ? NULL : ctrl_of(d->controller);
        if (bytes) {
            cancel_fn = readable_byte_cancel_fn(d->controller);
            source = readable_byte_source(d->controller);
        } else if (c) {
            cancel_fn = c->cancel_fn;
            source = c->source;
        }
        if (s->w.phase == 0) {
            /* §4.9.2's [[CancelSteps]]: whatever the controller was holding is dropped FIRST, so a source's
               `cancel` sees a stream with nothing left to give — §4.6's queue, or §4.7's queue AND its
               pending pull-intos. */
            if (bytes) readable_byte_clear(ctx, d->controller);
            if (stream_queue_reset(ctx, d) < 0) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, s->w.value);
            s->w.value = JS_UNDEFINED;
        }
        if (!JS_IsUndefined(cancel_fn)) {
            arg = s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED;
            r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), cancel_fn, source, 1, &arg, cb_result, &out,
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
        /* ClearAlgorithms: a cancelled controller pulls no more and cancels no more. */
        if (bytes) {
            readable_byte_clear_algorithms(ctx, d->controller);
        } else if (c) {
            rc_set(ctx, c, &c->pull_fn, JS_UNDEFINED);
            rc_set(ctx, c, &c->cancel_fn, JS_UNDEFINED);
        }
        STEP_GOTO(s->hdr.stage, CN_RESOLVE, &s->w.phase, NULL);
    }

    if (s->hdr.stage == CN_RESOLVE) {
        r = stream_promise_of_run(ctx, &s->w, s->reject_algorithm, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        STEP_GOTO(s->hdr.stage, CN_THEN, &s->w.phase, NULL);
    }

    if (s->hdr.stage == CN_THEN) {
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

    DCHECK(s->hdr.stage == CN_SETTLE, "the cancel machine resumed in a stage it never parks in");
    arg = s->w.value;
    r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->settle, JS_UNDEFINED, 1, &arg, cb_result, &out,
                      out_cb, out_argc);
    if (r > 0) return r;
    if (JS_IsException(out)) return JS_STEP_ABRUPT;
    JS_FreeValue(ctx, out);
    return JS_STEP_DONE;
}

#define CANCEL_DEF(i) { sizeof(JSCancelState), js_cancel_step, js_cancel_fini, (i), .catches_abrupt = 1, \
                        .visit = js_cancel_visit, \
                        .algorithm = "Streams §4.2/§4.4/§4.5 cancel(reason), through §4.9.1 " \
                                     "ReadableStreamCancel", .steps = CN_STEPS }
static const JSTrampStepDef js_cancel_defs[3] = {
    CANCEL_DEF(CANCEL_ON_STREAM), CANCEL_DEF(CANCEL_ON_DEFAULT), CANCEL_DEF(CANCEL_ON_BYOB),
};
#undef CANCEL_DEF

/* ---- §4.5's DEFAULT CONTROLLER MEMBERS -----------------------------------------------------------------------
 *
 * `enqueue`, `close` and `error` are each MACHINES, because each may answer a PARKED READ — and answering one
 * settles a promise, which is the page's code. `enqueue` additionally ends in CallPullIfNeeded, which calls the
 * page's `pull`. That is the whole reason this component is machines rather than functions. */
enum { CTRL_ENQUEUE = 0, CTRL_CLOSE, CTRL_ERROR };
/* WHERE THIS MACHINE RESTS, ACROSS §4.5's THREE MEMBERS. `enqueue`, `close` and `error` are one machine
   because they end in the same two operations — settling what a reader was waiting for, and asking whether to
   pull — and each stage names the step of whichever member reached it. */
#define CS_STAGES(X) \
    X(CS_START, "Streams §4.5 enqueue/close/error steps 1-2 (the brand and CanCloseOrEnqueue)") \
    X(CS_SIZE, "Streams §4.9.4 ReadableStreamDefaultControllerEnqueue step 3.1 (the strategy's size algorithm " \
               "over the chunk)") \
    X(CS_ENQUEUE, "Streams §4.9.4 ReadableStreamDefaultControllerEnqueue step 3.2 (EnqueueValueWithSize, whose " \
                  "own throw errors the controller)") \
    X(CS_SETTLE, "Streams §4.9.4 ReadableStreamDefaultControllerEnqueue step 2 (a waiting reader is answered " \
                 "with the chunk instead of it being queued)") \
    X(CS_SETTLE_ALL, "Streams §4.9.2 ReadableStreamClose / ReadableStreamError (settling `closed` and every " \
                     "parked read request, for `close()` and `error()`)") \
    X(CS_PULL, "Streams §4.9.4 ReadableStreamDefaultControllerCallPullIfNeeded (asked after every one of the " \
               "three members, because each changes what ShouldCallPull answers)") \
    X(CS_RETHROW, "Streams §4.9.4 ReadableStreamDefaultControllerEnqueue step 3.1's abrupt completion (the " \
                  "size algorithm's OWN throw is re-raised while the stream keeps the reason it was errored " \
                  "with)")
enum { CS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CS_STEPS[] = { CS_STAGES(JS_STEP_STAGE_LABEL) NULL };

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
    d = rs_stream_data(c->stream);
    DCHECK(d != NULL, "a controller's stream stopped being a ReadableStream");

    if (s->hdr.stage == CS_START) {
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
            s->w.func = rs_take_read(ctx, d, 0);
            if (JS_IsUndefined(s->w.func)) {
                STEP_GOTO(s->hdr.stage, JS_IsUndefined(c->size_fn) ? CS_ENQUEUE : CS_SIZE, &s->w.phase, NULL);
                s->size = 1;   /* the implicit strategy: one per chunk */
            } else {
                s->w.value = rs_read_result(ctx, JS_DupValue(ctx, a0), false);
                if (JS_IsException(s->w.value)) return JS_STEP_ABRUPT;
                STEP_GOTO(s->hdr.stage, CS_SETTLE, &s->w.phase, NULL);
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
            STEP_GOTO(s->hdr.stage, CS_SETTLE_ALL, &s->w.phase, NULL);
            s->w.settle = S_CLOSE_SET;
        } else {
            DCHECK(op == CTRL_ERROR, "a controller member ran with an operation this component does not have");
            if (d->state != RS_READABLE) return JS_STEP_DONE;
            s->w.err = JS_DupValue(ctx, a0);
            STEP_GOTO(s->hdr.stage, CS_SETTLE_ALL, &s->w.phase, NULL);
            s->w.settle = S_ERR_SET;
        }
    }

    if (s->hdr.stage == CS_SIZE) {
        JSValueConst chunk = s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED;
        r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), c->size_fn, JS_UNDEFINED, 1, &chunk, cb_result, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        cb_result = JS_UNDEFINED;
        if (JS_IsException(out)) goto size_bad;
        s->size = 0;
        if (JS_ToFloat64(ctx, &s->size, out) < 0) { JS_FreeValue(ctx, out); goto size_bad; }
        JS_FreeValue(ctx, out);
        /* §8.1's EnqueueValueWithSize: a size that is not a FINITE NON-NEGATIVE number is a RangeError, and
           §4.5 answers both that and a throwing size the same way — error the stream, then re-raise, so the
           caller of enqueue() sees the failure AND every reader sees the stream die. */
        if (!isfinite(s->size) || s->size < 0) {
            JS_ThrowRangeError(ctx, "a queuing strategy's size must be a finite, non-negative number");
        size_bad:
            s->w.err = JS_GetException(ctx);
            s->rethrow = JS_DupValue(ctx, s->w.err);
            s->w.settle = S_ERR_SET;
            STEP_GOTO(s->hdr.stage, CS_RETHROW, &s->w.phase, NULL);
            s->w.pull = P_IDLE;
        } else {
            STEP_GOTO(s->hdr.stage, CS_ENQUEUE, &s->w.phase, NULL);
        }
    }
    if (s->hdr.stage == CS_ENQUEUE) {
        JSValueConst chunk = s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        stream_enqueue(ctx, d, chunk, s->size);
        STEP_GOTO(s->hdr.stage, CS_PULL, &s->w.phase, NULL);
    }
    if (s->hdr.stage == CS_RETHROW) {
        /* the stream errors first — every parked read is rejected — and only then does enqueue() itself
           re-raise, which is the order §4.5 states and the order a page observes */
        r = rs_settle_run(ctx, &s->w, d, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        JS_Throw(ctx, s->rethrow);
        s->rethrow = JS_UNDEFINED;
        return JS_STEP_ABRUPT;
    }

    if (s->hdr.stage == CS_SETTLE) {
        arg = s->w.value;
        r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->w.func, JS_UNDEFINED, 1, &arg, cb_result, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, out);
        cb_result = JS_UNDEFINED;
        STEP_GOTO(s->hdr.stage, CS_PULL, &s->w.phase, NULL);
    }
    if (s->hdr.stage == CS_SETTLE_ALL) {
        r = rs_settle_run(ctx, &s->w, d, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        return JS_STEP_DONE;
    }

    DCHECK(s->hdr.stage == CS_PULL, "a controller member resumed in a stage it never parks in");
    r = ctrl_pull_run(ctx, &s->w, s->hdr.this_val, cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    return JS_STEP_DONE;
}

#define CTRL_DEF(i) { sizeof(JSCtrlState), js_ctrl_step, NULL, (i), \
                      .algorithm = "Streams §4.5 enqueue(chunk) / close() / error(e)", .steps = CS_STEPS, \
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

/* §4.9.4's ReadableStreamDefaultControllerGetDesiredSize. With the default strategy the high-water mark is 1, so
   it is 1 minus what is queued — a page uses it to decide whether to enqueue more, and answering a constant
   would make that decision wrong. An errored stream has no room to describe, which is why null is a value here
   and not an error. */
static JSValue ctrl_desired_size(JSContext *ctx, ControllerData *c)
{
    StreamData *d = rs_stream_data(c->stream);
    DCHECK(d != NULL, "a controller's stream stopped being a ReadableStream");
    if (d->state == RS_ERRORED) return JS_NULL;
    if (d->state == RS_CLOSED) return JS_NewInt32(ctx, 0);
    return JS_NewFloat64(ctx, c->hwm - d->queue_total);
}

/* §4.5's `desiredSize` member — the brand test, then the operation. */
static JSValue js_ctrl_desired(JSContext *ctx, JSValueConst this_val, int magic)
{
    ControllerData *c = ctrl_of(this_val);
    (void)magic;
    if (!c) return JS_ThrowTypeError(ctx, "not a ReadableStreamDefaultController");
    return ctrl_desired_size(ctx, c);
}

JSValue readable_ctrl_desired_size(JSContext *ctx, JSValueConst ctrl)
{
    ControllerData *c = ctrl_of(ctrl);
    DCHECK(c != NULL, "the desired size of something that is not a §4.5 controller was asked for");
    return ctrl_desired_size(ctx, c);
}

/* ---- THE ABSTRACT OPERATIONS §4 PERFORMS ON ITSELF ---------------------------------------------------------- */

/* §4.2.5, §4.2 AND FETCH PERFORM THE ABSTRACT OPERATIONS, so the async iterator, the tee and the drain keep the
   function objects INSTALLED at each realm's build — a page that rebinds the members changes what its own calls
   do and changes nothing about `for await`, `tee()` or Fetch's "clone a body".
   PER REALM: a function object carries the realm it was minted in, so one set held for the agent ran a child
   document's every read, cancel and release through the first document's realm. */
enum { RSF_READ = 0, RSF_RELEASE, RSF_CANCEL, RSF_GET_READER, RSF_TEE, RSF_TEE_CLONE, RSF_N };
static int g_rs_fn_slot[RSF_N];
static int g_ctrl_fn_slot[RS_CTRL_N];

static JSValue rs_fn(JSContext *ctx, int which)        /* OWNED */
{
    DCHECK(which >= 0 && which < RSF_N, "a stream operation was asked for by a name this component does not map");
    return realm_value_get(ctx, g_rs_fn_slot[which]);
}

static JSValue rs_ctrl_fn(JSContext *ctx, int which)   /* OWNED */
{
    DCHECK(which >= 0 && which < RS_CTRL_N,
           "a §4.5 controller operation was asked for by a name this component does not map");
    return realm_value_get(ctx, g_ctrl_fn_slot[which]);
}

/* ---- §4.2.5's ASYNCHRONOUS ITERATION --------------------------------------------------------------------------
 *
 * `for await (const chunk of stream)`. §4.2.1's IDL declares
 * `async_iterable<any>(optional ReadableStreamIteratorOptions options = {})`, and Web IDL §3.7.10 owns the whole
 * JavaScript binding of that declaration — `values`, %Symbol.asyncIterator%, §3.7.10.1's default asynchronous
 * iterator object with its `ongoing promise` and `is finished`, and §3.7.10.2's prototype with its `next` and
 * its `return`. core/idl_async_iter.c is where that is written ONCE, for every interface that declares one.
 *
 * WHAT IS §4.2.5'S IS THE THREE ALGORITHMS §2.5.10 ASKS THE PROSE FOR, and they are the three functions below.
 * This file used to carry the other half as well — an eight-entry machine that re-derived §3.7.10.2's chaining
 * rule, its `is finished` short circuit and its iteration-result wrapping — which was one object answering
 * `next()` differently depending on which copy of that algorithm owned it. There is one copy now.
 *
 * IT REACHES THE OPERATIONS, NOT THE MEMBERS. §4.2.5 performs ReadableStreamDefaultReaderRead,
 * ReadableStreamReaderGenericCancel and ReadableStreamDefaultReaderRelease, which are abstract operations, and a
 * page that replaces ReadableStreamDefaultReader.prototype.read must not thereby change what `for await` does —
 * the corpus asserts exactly that. So this reaches the FUNCTION OBJECTS this component installed, with no
 * property read in between.
 *
 * THE ITERATOR'S OWN STATE — §4.2.5's "iterator's reader" and "iterator's prevent cancel" — lives in the ONE
 * slot Web IDL §3.7.10.1 gives a component, and it is a JS VALUE for the reason §State-isolation gives: its
 * writes are property writes the COW delta already captures, and it parks to the cold tier with the flow that
 * owns it. A malloc'd C record would revert a POINTER on a context switch and leave the reader reachable from
 * nothing. */
enum { RSI_ST_READER = 0, RSI_ST_PREVENT_CANCEL };

/* §4.2.1: `dictionary ReadableStreamIteratorOptions { boolean preventCancel = false; };` */
static const IdlDictMember ITERATOR_OPTIONS[] = {
    { "preventCancel", IDL_BOOLEAN },
};
static const IdlArgType ITERATOR_ARGS[1] = { IDL_DICT };

static int g_rs_iter_handle = -1;

/* §4.2.5's "iterator's reader", off that slot. OWNED. */
static JSValue rsi_reader(JSContext *ctx, JSValueConst state)
{
    return JS_GetPropertyUint32(ctx, state, RSI_ST_READER);
}

/* §4.2.5's "iterator's prevent cancel". */
static bool rsi_prevent_cancel(JSContext *ctx, JSValueConst state)
{
    JSValue v = JS_GetPropertyUint32(ctx, state, RSI_ST_PREVENT_CANCEL);
    bool b = JS_ToBool(ctx, v) != 0;

    JS_FreeValue(ctx, v);
    return b;
}

/* THE THREE ALGORITHMS' OWN STEP STORAGE. ONE declaration, because core/idl_async_iter.c keeps one block for
   whichever of them is running and ZEROES it before that algorithm's first entry.
   THERE IS NO CURSOR BYTE HERE. There was one — `at` — and it was a STAGE wearing the wrong clothes: the three
   algorithms rest at §4.2.5's steps, and a byte in this record is invisible to the driver's assert, unnameable
   at a park (a flow suspended inside AcquireReadableStreamDefaultReader reported "Web IDL §3.7.10.2 return step
   8.4") and unresolvable back to a step by the build that resumes it. The stages below are the machine's own,
   joined onto Web IDL's at the declaration. What remains here are the sub-sequence cursors inside one stage,
   which is exactly what stream_work.h's `phase`, `settle` and `pull` are. */
typedef struct {
    StreamWork w;
    JSValue    reader;    /* §4.2.5's "iterator's reader", taken off the state slot at each algorithm's step 1 */
    JSValue    promise;   /* the next algorithm's step 3 promise, or the return algorithm's step 4.1 result */
    JSValue    resolve;   /* that promise's capability, held until step 4's read request captures it */
    JSValue    reject;
} RsIterWork;

static void rs_iter_visit(JSContext *ctx, void *work, JSStepVisit *v)
{
    RsIterWork *k = work;

    stream_work_visit(ctx, &k->w, v);
    v->val(ctx, &k->reader);
    v->val(ctx, &k->promise);
    v->val(ctx, &k->resolve);
    v->val(ctx, &k->reject);
}

/* EVERY OWNED FIELD IN PLACE BEFORE THE FIRST THING THAT CAN THROW — the failure path tears the whole machine
   down through the ownership declaration above, which frees exactly what it names, and a zeroed slot reads as
   the INTEGER 0 rather than as undefined. */
static void rs_iter_work_start(RsIterWork *k)
{
    stream_work_start(&k->w);
    k->reader = k->promise = k->resolve = k->reject = JS_UNDEFINED;
}

/* §4.2.5: "The asynchronous iterator initialization steps for a ReadableStream, given stream, iterator, and
   args, are: 1. Let reader be ? AcquireReadableStreamDefaultReader(stream). 2. Set iterator's reader to reader.
   3. Let preventCancel be args[0]["preventCancel"]. 4. Set iterator's prevent cancel to preventCancel."
   A STEP because step 1 is a CALL: §4.9.1's AcquireReadableStreamDefaultReader reaches §4.9.3's
   ReadableStreamReaderGenericInitialize, which SETTLES the reader's `closed` promise at once on a stream
   that has already closed or errored, and a resolving function is the page's code — which is the ONE place
   these steps rest, and therefore the one stage they declare. It is numbered from
   IDL_ASYNC_ITER_INIT_STEP_FIRST because Web IDL §3.7.10 step 3.1.6 is what runs it, and joined onto that
   member's list at the declaration. */
#define RSI_INIT_STAGES(X) \
    X(RSI_INIT_ACQUIRE, \
      "Streams §4.2.5 asynchronous iterator initialization steps step 1 (reader is ? " \
      "AcquireReadableStreamDefaultReader(stream), whose §4.9.3 GenericInitialize settles the reader's closed " \
      "at once on a stream that has already closed or errored)")
enum { IDL_ASYNC_ITER_INIT_STAGE_BASE(RSI_INIT_STAGES) RSI_INIT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RSI_INIT_STEPS[] = { RSI_INIT_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int rs_iter_init(JSContext *ctx, JSStepHdr *hdr, void *work, JSValueConst target, JSValueConst iter,
                        int argc, JSValueConst *argv, JSValue *pstate, JSValue in,
                        JSValue **out_cb, int *out_argc)
{
    RsIterWork *k = work;
    JSValue reader, state;
    int r;

    (void)iter;
    /* A STAGE BELOW THE FIRST OF THESE IS THE HOSTING MEMBER'S, which is what a first entry looks like: Web IDL
       §3.7.10 step 3.1.6 is the stage that RUNS these steps, and it is one lower than theirs. */
    if (hdr->stage < IDL_ASYNC_ITER_INIT_STEP_FIRST) {
        rs_iter_work_start(k);
        STEP_GOTO(hdr->stage, RSI_INIT_ACQUIRE, &k->w.phase, NULL);
    }
    DCHECK(hdr->stage == RSI_INIT_ACQUIRE,
           "§4.2.5's asynchronous iterator initialization steps resumed at a step they never rest at");
    {
        /* step 1: AcquireReadableStreamDefaultReader(stream) — `getReader()` with no options IS that
           operation, reached through the function object this component installed. The `?` is why a LOCKED
           stream's TypeError is the member's throw rather than a rejected promise: this returns <0 and Web IDL
           §3.7.10 step 3.1.6 hands the throw straight out of `values()`. */
        JSValue fn = rs_fn(ctx, RSF_GET_READER);

        r = step_call_run(ctx, &k->w.phase, STEP_CB(k->w.cb), fn, target, 0, NULL, in, &reader,
                          out_cb, out_argc);
        JS_FreeValue(ctx, fn);
    }
    if (r > 0) return r;
    if (JS_IsException(reader)) return -1;

    state = JS_NewArray(ctx);
    if (JS_IsException(state)) { JS_FreeValue(ctx, reader); return -1; }
    /* Two defines and two checks, because each CONSUMES its value: folded into one `||` the second is never
       handed over when the first fails, and leaks. */
    if (JS_DefinePropertyValueUint32(ctx, state, RSI_ST_READER, reader, JS_PROP_C_W_E) < 0) {  /* step 2 */
        JS_FreeValue(ctx, state);
        return -1;
    }
    /* steps 3-4. `optional ReadableStreamIteratorOptions options = {}` means the dictionary is there even when
       the page passed nothing, and its `preventCancel` has `= false` written in the IDL — so the read is
       total and there is no "absent" state to model. */
    if (JS_DefinePropertyValueUint32(ctx, state, RSI_ST_PREVENT_CANCEL,
                                     JS_NewBool(ctx, argc > 0 && idl_dict_bool(ctx, argv[0], "preventCancel")),
                                     JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, state);
        return -1;
    }
    DCHECK(JS_IsUndefined(*pstate),
           "§4.2.5's initialization steps ran on an iterator that already had a reader — Web IDL §3.7.10 step "
           "3.1.6 runs them exactly once, at the mint");
    *pstate = state;
    return 0;
}

/* §4.2.5's READ REQUEST, as the two reactions this component's read promise delivers its three item lists
 * through: "4. Let readRequest be a new read request with the following items: chunk steps, given chunk —
 * Resolve promise with chunk; close steps — Perform ! ReadableStreamDefaultReaderRelease(reader), Resolve
 * promise with end of iteration; error steps, given e — Perform ! ReadableStreamDefaultReaderRelease(reader),
 * Reject promise with e."
 *
 * THE RELEASE COMES FIRST IN TWO OF THE THREE, and it is a machine (it rejects the reader's `closed` promise
 * and every request the reader had parked), so it is a stage of its own ahead of the settle. */
enum { RSI_READ_OK = 0, RSI_READ_ERR, RSI_RXN_N };

#define RSIX_STAGES(X) \
    X(RSIX_START, "Streams §4.2.5 get the next iteration result step 4 (which of the read request's chunk, " \
                  "close and error steps this settlement is)") \
    X(RSIX_RELEASE, "Streams §4.2.5 get the next iteration result steps 4.2.1 and 4.3.1 (Perform ! " \
                    "ReadableStreamDefaultReaderRelease(reader))") \
    X(RSIX_SETTLE, "Streams §4.2.5 get the next iteration result steps 4.1.1, 4.2.2 and 4.3.2 (resolving " \
                   "promise with the chunk or with end of iteration, or rejecting it with e)")
enum { RSIX_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RSIX_STEPS[] = { RSIX_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;
    /* w.func is the capability function this settlement calls, w.value what it settles with — the same two
       slots §2.4.1's iteration uses for the same purpose. */
    StreamWork w;
    JSValue    reader;
} JSRsIterRxnState;

static void js_rs_iter_rxn_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSRsIterRxnState *s = st;

    stream_work_visit(ctx, &s->w, v);
    v->val(ctx, &s->reader);
}

static int js_rs_iter_rxn_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSRsIterRxnState *s = st;
    JSValueConst arg;
    JSValue out;
    int r;

    if (s->hdr.stage == RSIX_START) {
        DCHECK(s->hdr.arg == RSI_READ_OK || s->hdr.arg == RSI_READ_ERR,
               "a §4.2.5 read request ran with an item list the standard does not give it");
        stream_work_start(&s->w);
        s->reader = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0));
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (s->hdr.arg == RSI_READ_ERR) {
            /* error steps, given e */
            s->w.func = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 2));
            s->w.value = JS_DupValue(ctx, step_arg(&s->hdr, 0));
            STEP_GOTO(s->hdr.stage, RSIX_RELEASE, &s->w.phase, NULL);
        } else {
            JSValueConst result = step_arg(&s->hdr, 0);
            JSValue done_v;
            int done;

            DCHECK(JS_IsObject(result),
                   "§4.4's read() fulfilled with something that is not the iteration result object it built");
            done_v = JS_GetPropertyStr(ctx, result, "done");
            done = JS_ToBool(ctx, done_v);
            JS_FreeValue(ctx, done_v);
            s->w.func = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 1));
            if (done) {
                /* close steps: release, and then resolve with Web IDL §2.5.10's END OF ITERATION — the marker
                   outside the JavaScript value space, not `undefined`, which `async_iterable<any>` may
                   legitimately yield as a chunk. */
                s->w.value = idl_async_iter_end(ctx);
                if (JS_IsException(s->w.value)) { s->w.value = JS_UNDEFINED; return JS_STEP_ABRUPT; }
                STEP_GOTO(s->hdr.stage, RSIX_RELEASE, &s->w.phase, NULL);
            } else {
                /* chunk steps, given chunk: "Resolve promise with chunk" — the CHUNK, and not the
                   { value, done } object §4.4's read() wrapped it in. §3.7.10.2's fulfillSteps reads what the
                   declaration's type says, and `async_iterable<any>` says the chunk. */
                s->w.value = JS_GetPropertyStr(ctx, result, "value");
                if (JS_IsException(s->w.value)) { s->w.value = JS_UNDEFINED; return JS_STEP_ABRUPT; }
                STEP_GOTO(s->hdr.stage, RSIX_SETTLE, &s->w.phase, NULL);
            }
        }
    }

    if (s->hdr.stage == RSIX_RELEASE) {
        JSValue fn = rs_fn(ctx, RSF_RELEASE);

        r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), fn, s->reader, 0, NULL, cb_result, &out,
                          out_cb, out_argc);
        JS_FreeValue(ctx, fn);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, out);
        cb_result = JS_UNDEFINED;
        STEP_GOTO(s->hdr.stage, RSIX_SETTLE, &s->w.phase, NULL);
    }

    DCHECK(s->hdr.stage == RSIX_SETTLE, "a §4.2.5 read request resumed in a stage it never parks in");
    arg = s->w.value;
    r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->w.func, JS_UNDEFINED, 1, &arg, cb_result, &out,
                      out_cb, out_argc);
    if (r > 0) return r;
    if (JS_IsException(out)) return JS_STEP_ABRUPT;
    JS_FreeValue(ctx, out);
    return JS_STEP_DONE;
}

/* No fini: a read request's items answer nothing — what a caller sees is the promise step 3 created and
   step 6 returned, which the machine settles. */
#define RSI_RXN_DEF(i) { sizeof(JSRsIterRxnState), js_rs_iter_rxn_step, NULL, (i), \
                         .catches_abrupt = 1, .visit = js_rs_iter_rxn_visit, \
                         .algorithm = "Streams §4.2.5 the ReadableStream async iterator's read request", \
                         .steps = RSIX_STEPS }
static const JSTrampStepDef js_rs_iter_rxn_defs[RSI_RXN_N] = {
    RSI_RXN_DEF(RSI_READ_OK), RSI_RXN_DEF(RSI_READ_ERR),
};
#undef RSI_RXN_DEF

static int g_rsi_stepids[RSI_RXN_N];

/* §4.2.5: "The get the next iteration result steps for a ReadableStream, given stream and iterator, are:
   1. Let reader be iterator's reader. 2. Assert: reader.[[stream]] is not undefined. 3. Let promise be a new
   promise. 4. Let readRequest be a new read request with the following items ... 5. Perform !
   ReadableStreamDefaultReaderRead(this, readRequest). 6. Return promise."

   WHERE THESE TWO ALGORITHMS REST — ONE list, because Web IDL §3.7.10.2 runs both of them on ONE machine and a
   machine has ONE step list. They are numbered from IDL_ASYNC_ITER_STEP_FIRST and joined onto that machine's
   own stages at the declaration, so a flow parked inside the cancel reports §4.2.5's step and not §3.7.10.2's
   return step 8.4, which is the step that RUNS this algorithm rather than the one being run. */
#define RSI_STAGES(X) \
    X(RSI_NEXT_READ, \
      "Streams §4.2.5 get the next iteration result step 5 (Perform ! ReadableStreamDefaultReaderRead(this, " \
      "readRequest))") \
    X(RSI_RET_CANCEL, \
      "Streams §4.2.5 asynchronous iterator return step 4.1 (result is ! " \
      "ReadableStreamReaderGenericCancel(reader, arg))") \
    X(RSI_RET_RELEASE, \
      "Streams §4.2.5 asynchronous iterator return steps 4.2 and 5 (Perform ! " \
      "ReadableStreamDefaultReaderRelease(reader), on whichever arm of step 4's condition this is)") \
    X(RSI_RET_RESOLVE, \
      "Streams §4.2.5 asynchronous iterator return step 6 (a promise resolved with undefined)")
enum { IDL_ASYNC_ITER_STAGE_BASE(RSI_STAGES) RSI_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RSI_STEPS[] = { RSI_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int rs_iter_next(JSContext *ctx, JSStepHdr *hdr, void *work, JSValueConst target, JSValueConst iter,
                        JSValue *pstate, JSValue in, JSValue *ppromise, JSValue **out_cb, int *out_argc)
{
    RsIterWork *k = work;
    JSValueConst data[3];
    JSValue out;
    int r;

    (void)iter; (void)target;
    /* A STAGE BELOW THE FIRST OF THESE IS §3.7.10.2's — its next step 8.4, the stage that RUNS this algorithm,
       which is what a first entry looks like. */
    if (hdr->stage < IDL_ASYNC_ITER_STEP_FIRST) {
        JSValue funcs[2];
        ReaderData *rd;

        rs_iter_work_start(k);
        k->reader = rsi_reader(ctx, *pstate);                         /* step 1 */
        rd = rs_reader_data(k->reader);
        /* step 2: "Assert: reader.[[stream]] is not undefined." Web IDL §3.7.10.2's next step 8.2
           short-circuits a FINISHED iterator, and it is the close and error steps above — the only two that
           release this reader — whose end-of-iteration and rejection finish it. So a released reader here is a
           next() that reached these steps after one of them, which the chaining rule makes impossible. */
        DCHECK(rd != NULL && !JS_IsUndefined(rd->stream),
               "§4.2.5's get the next iteration result step 2 found an iterator whose reader has already been "
               "released");
        (void)rd;
        k->promise = JS_NewPromiseCapability(ctx, funcs);             /* step 3 */
        if (JS_IsException(k->promise)) return -1;
        k->resolve = funcs[0];
        k->reject = funcs[1];
        STEP_GOTO(hdr->stage, RSI_NEXT_READ, &k->w.phase, NULL);
    }
    DCHECK(hdr->stage == RSI_NEXT_READ,
           "§4.2.5's get the next iteration result resumed at a step it never rests at");
    {
        /* step 5: ReadableStreamDefaultReaderRead — `read()` IS that operation, reached through the function
           object this component installed. */
        JSValue fn = rs_fn(ctx, RSF_READ);

        r = step_call_run(ctx, &k->w.phase, STEP_CB(k->w.cb), fn, k->reader, 0, NULL, in, &out,
                          out_cb, out_argc);
        JS_FreeValue(ctx, fn);
    }
    if (r > 0) return r;
    if (JS_IsException(out)) return -1;
    /* step 4's item lists, attached to what the read answered: the promise's own capability rides along,
       because a read request's steps settle the promise step 3 created and nothing else. */
    data[0] = k->reader; data[1] = k->resolve; data[2] = k->reject;
    r = stream_react(ctx, out, g_rsi_stepids[RSI_READ_OK], g_rsi_stepids[RSI_READ_ERR], data, 3);
    JS_FreeValue(ctx, out);
    if (r < 0) return -1;
    *ppromise = k->promise;                                           /* step 6 */
    k->promise = JS_UNDEFINED;
    return 0;
}

/* §4.2.5: "The asynchronous iterator return steps for a ReadableStream, given stream, iterator, and arg, are:
   1. Let reader be iterator's reader. 2. Assert: reader.[[stream]] is not undefined. 3. Assert:
   reader.[[readRequests]] is empty, as the async iterator machinery guarantees that any previous calls to
   next() have settled before this is called. 4. If iterator's prevent cancel is false: 4.1. Let result be !
   ReadableStreamReaderGenericCancel(reader, arg). 4.2. Perform ! ReadableStreamDefaultReaderRelease(reader).
   4.3. Return result. 5. Perform ! ReadableStreamDefaultReaderRelease(reader). 6. Return a promise resolved
   with undefined."
   Step 4.3 is the whole reason a return algorithm is declared at all: the promise the member hands back settles
   with the SOURCE's cancel promise rather than ahead of it, and §2.5.10 passes a rejection out of it on to the
   caller unchanged.
   ITS STAGES ARE THE ONES DECLARED BESIDE THE NEXT ALGORITHM'S, for the reason stated there: one machine, one
   step list. */
static int rs_iter_return(JSContext *ctx, JSStepHdr *hdr, void *work, JSValueConst target, JSValueConst iter,
                          JSValue *pstate, JSValueConst value, JSValue in,
                          JSValue *ppromise, JSValue **out_cb, int *out_argc)
{
    RsIterWork *k = work;
    JSValue out;
    int r;

    (void)iter; (void)target;
    /* A STAGE BELOW THE FIRST OF THESE IS §3.7.10.2's — its return step 8.4, the stage that RUNS this
       algorithm, which is what a first entry looks like. */
    if (hdr->stage < IDL_ASYNC_ITER_STEP_FIRST) {
        rs_iter_work_start(k);
        k->reader = rsi_reader(ctx, *pstate);                         /* step 1 */
        {
            /* steps 2-3, the two assertions the standard states. They are computed here rather than inside the
               DCHECKs because both accessors CAPTURE their record into the running flow's COW delta, and a
               condition a release build does not evaluate must not be the only place that happens. */
            ReaderData *rd = rs_reader_data(k->reader);
            StreamData *d = rd && !JS_IsUndefined(rd->stream) ? rs_stream_data(rd->stream) : NULL;
            uint32_t parked = d ? rs_read_pending(ctx, d) : 1;

            DCHECK(d != NULL,
                   "§4.2.5's asynchronous iterator return step 2 found an iterator whose reader has already "
                   "been released — Web IDL §3.7.10.2's return step 8.2 short-circuits a finished iterator");
            DCHECK(parked == 0,
                   "§4.2.5's asynchronous iterator return step 3 found a read request still parked — Web IDL "
                   "§3.7.10.2 steps 9-11 chain this call onto the ongoing promise, so every previous next() "
                   "has settled before these steps run");
            (void)d; (void)parked;
        }
        /* step 4's condition, decided once, with every cursor at rest. */
        STEP_GOTO(hdr->stage, rsi_prevent_cancel(ctx, *pstate) ? RSI_RET_RELEASE : RSI_RET_CANCEL,
                  &k->w.phase, NULL);
    }

    if (hdr->stage == RSI_RET_CANCEL) {
        /* step 4.1: ReadableStreamReaderGenericCancel(reader, arg) — the reader's `cancel()` IS that
           operation, reached through the function object this component installed. */
        JSValue fn = rs_fn(ctx, RSF_CANCEL);

        r = step_call_run(ctx, &k->w.phase, STEP_CB(k->w.cb), fn, k->reader, 1, &value, in, &out,
                          out_cb, out_argc);
        JS_FreeValue(ctx, fn);
        if (r > 0) return r;
        if (JS_IsException(out)) return -1;
        JS_FreeValue(ctx, k->promise);
        k->promise = out;          /* step 4.3's result, handed over once step 4.2's release has run */
        /* AND IT IS WHAT TELLS THE RELEASE BELOW WHICH ARM IT IS ON — step 4.2's or step 5's — so the two
           arms are not re-derived from `prevent cancel` on a resume, which is the decision-taken-twice this
           engine's stage rule forbids. That works only because step 4.1 always answers with a promise. */
        DCHECK(JS_IsObject(k->promise),
               "§4.2.5's asynchronous iterator return step 4.1 answered with something that is not a promise — "
               "ReadableStreamReaderGenericCancel returns one for every input");
        in = JS_UNDEFINED;
        STEP_GOTO(hdr->stage, RSI_RET_RELEASE, &k->w.phase, NULL);
    }

    if (hdr->stage == RSI_RET_RELEASE) {
        JSValue fn = rs_fn(ctx, RSF_RELEASE);   /* steps 4.2 and 5 */

        r = step_call_run(ctx, &k->w.phase, STEP_CB(k->w.cb), fn, k->reader, 0, NULL, in, &out,
                          out_cb, out_argc);
        JS_FreeValue(ctx, fn);
        if (r > 0) return r;
        if (JS_IsException(out)) return -1;
        JS_FreeValue(ctx, out);
        in = JS_UNDEFINED;
        if (!JS_IsUndefined(k->promise)) {
            *ppromise = k->promise;                                   /* step 4.3 */
            k->promise = JS_UNDEFINED;
            return 0;
        }
        STEP_GOTO(hdr->stage, RSI_RET_RESOLVE, &k->w.phase, NULL);
    }

    DCHECK(hdr->stage == RSI_RET_RESOLVE,
           "§4.2.5's asynchronous iterator return steps resumed at a step they never rest at");
    /* step 6: "Return a promise resolved with undefined." PromiseResolve, as the sub-sequence every §4/§5
       "react to what this returned" begins with — `w.value` is the undefined rs_iter_work_start left. */
    r = stream_promise_of_run(ctx, &k->w, 0, in, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return -1;
    *ppromise = k->w.func;
    k->w.func = JS_UNDEFINED;
    return 0;
}

/* §4.2.1: `async_iterable<any>(optional ReadableStreamIteratorOptions options = {});` — ONE type parameter, so
   it is a VALUE asynchronously iterable declaration and Web IDL §3.7.10 gives the prototype `values` and
   %Symbol.asyncIterator% and neither `entries` nor `keys`. */
/* DESIGNATED, and every declaration of this shape must be: the struct has gained fields twice, and a
   POSITIONAL initializer re-aims every value after the new one — silently wherever the two types happen to
   agree, which two adjacent pointer fields always do. Naming each one is what makes a field added tomorrow
   land nowhere rather than one slot early. */
static const IdlAsyncIterOps RS_ITER_OPS = {
    .iface = "ReadableStream",
    .pair = false,
    .implements = readable_stream_is,
    .init = rs_iter_init,
    .next = rs_iter_next,
    .ret = rs_iter_return,
    /* the three algorithms' own rest points, joined onto the two Web IDL machines that host them */
    .init_steps = RSI_INIT_STEPS,
    .steps = RSI_STEPS,
    .work_size = sizeof(RsIterWork),
    .work_visit = rs_iter_visit,
    .arg_types = ITERATOR_ARGS, .nargs = 1,
    .members = ITERATOR_OPTIONS, .nmembers = (int)(sizeof ITERATOR_OPTIONS / sizeof *ITERATOR_OPTIONS)
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
    /* §4.9.1's cloneForBranch2. The public `tee()` never sets it — both branches get the SAME chunk, which is
       what the standard says and what a page teeing its own stream expects. Fetch §2.2.4 "Bodies"'s "clone a body" DOES:
       `response.clone()` must give the second branch a value the first cannot reach, or a page that mutates
       the chunk it read has changed what the clone will read. Fourteen of response-clone's subtests are
       exactly that assertion, one per BufferSource type. */
    uint8_t clone_for_branch2;
} TeeData;

static JSClassID g_tee_class;
static int       g_tee_stepids[TEE_N];
/* The controller members, as function objects — the same reason §4.4 holds the reader's. */

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

static const uint16_t TEE_VALS[] = {
    RS_OFF(TeeData, reader), RS_OFF(TeeData, branch[0]), RS_OFF(TeeData, branch[1]),
    RS_OFF(TeeData, ctrl[0]), RS_OFF(TeeData, ctrl[1]), RS_OFF(TeeData, cancel_promise),
    RS_OFF(TeeData, cancel_funcs[0]), RS_OFF(TeeData, cancel_funcs[1]),
    RS_OFF(TeeData, reason[0]), RS_OFF(TeeData, reason[1]),
};
static const CowRecord TEE_REC = { sizeof(TeeData), TEE_VALS, RS_NVAL(TEE_VALS) };

static TeeData *tee_of(JSValueConst v)
{
    TeeData *t = JS_GetOpaque(v, g_tee_class);
    if (t) cow_capture_host_record(v, t, &TEE_REC);
    return t;
}

/* WHERE THIS MACHINE RESTS, AS §4.9.1 NUMBERS IT. One machine over ReadableStreamDefaultTee's pullAlgorithm,
   its two cancelAlgorithms and the read reactions they share. */
#define TS_STAGES(X) \
    X(TS_START, "Streams §4.9.1 ReadableStreamDefaultTee (which of pullAlgorithm, cancel1Algorithm, " \
                "cancel2Algorithm or a read reaction this entry is)") \
    X(TS_READ, "Streams §4.9.1 ReadableStreamDefaultTee's pullAlgorithm step 3 (one read through the shared " \
               "reader)") \
    X(TS_B0, "Streams §4.9.1 ReadableStreamDefaultTee's chunkSteps step 5 (enqueue the chunk into branch 1)") \
    X(TS_B1, "Streams §4.9.1 ReadableStreamDefaultTee's chunkSteps step 6 (enqueue it into branch 2)") \
    X(TS_RESOLVE_CANCEL, "Streams §4.9.1 ReadableStreamDefaultTee's cancelAlgorithm step 4.2 (resolve " \
                         "cancelPromise with the result of cancelling the source)") \
    X(TS_CANCEL_SOURCE, "Streams §4.9.1 ReadableStreamDefaultTee's cancelAlgorithm step 4.1 " \
                        "(ReadableStreamCancel on the source, once BOTH branches have cancelled)") \
    X(TS_CANCEL_ADOPT, "Streams §4.9.1 ReadableStreamDefaultTee's cancelAlgorithm step 5 (this branch's " \
                       "answer is cancelPromise)")
enum { TS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TS_STEPS[] = { TS_STAGES(JS_STEP_STAGE_LABEL) NULL };
enum { CF_ENQUEUE = 0, CF_CLOSE, CF_ERROR };

typedef struct {
    JSStepHdr hdr;
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
    (void)ctx;
    if (take_result) s->result = JS_UNDEFINED;
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

    {
        JSValue fn = rs_ctrl_fn(ctx, which);
        r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), fn, t->ctrl[i], argc, &arg, in, &out,
                          out_cb, out_argc);
        JS_FreeValue(ctx, fn);
    }
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

    if (s->hdr.stage == TS_START) {
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
            STEP_GOTO(s->hdr.stage, TS_READ, &s->phase, NULL);
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
            STEP_GOTO(s->hdr.stage, TS_CANCEL_SOURCE, &s->phase, NULL);
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
            STEP_GOTO(s->hdr.stage, TS_B0, &s->phase, NULL);
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
            STEP_GOTO(s->hdr.stage, TS_B0, &s->phase, NULL);
            break;
        }
    }

    t = tee_of(s->tee);
    DCHECK(t != NULL, "a tee machine's record stopped being one");

again:
    if (s->hdr.stage == TS_READ) {
        JSValueConst data[1];
        {
            JSValue fn = rs_fn(ctx, RSF_READ);
            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), fn, t->reader, 0, NULL, cb_result, &out,
                              out_cb, out_argc);
            JS_FreeValue(ctx, fn);
        }
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        data[0] = s->tee;
        r = stream_react(ctx, out, g_tee_stepids[TEE_READ_OK], g_tee_stepids[TEE_READ_ERR], data, 1);
        JS_FreeValue(ctx, out);
        return r < 0 ? JS_STEP_ABRUPT : JS_STEP_DONE;
    }

    if (s->hdr.stage == TS_B0 || s->hdr.stage == TS_B1) {
        int i = s->hdr.stage == TS_B1;
        for (; i < 2; i++) {
            STEP_GOTO(s->hdr.stage, i ? TS_B1 : TS_B0, &s->phase, NULL);
            /* §4.2: a branch the page has already cancelled is not fed, closed or errored. */
            if (s->done != 2 && t->canceled[i]) continue;
            /* §4.9.1 step 13.2: BRANCH 2 gets a STRUCTURED CLONE of the chunk when the flag is set, and a
               chunk that cannot be cloned errors BOTH branches rather than one — the two are one tee, and
               leaving branch 1 alive with a value branch 2 never got is the split the flag exists to prevent. */
            if (i == 1 && t->clone_for_branch2 && s->done == 0 && !JS_IsUndefined(s->value)) {
                JSValue c = structured_clone(ctx, s->value);
                if (JS_IsException(c)) {
                    JSValue e = JS_GetException(ctx);
                    JS_FreeValue(ctx, s->value);
                    s->value = e;
                    s->done = 2;          /* both branches error, with the clone's own reason */
                    i = 0;                /* restart the pair: branch 1 has not been errored yet */
                    STEP_GOTO(s->hdr.stage, TS_B0, &s->phase, NULL);
                    continue;
                }
                JS_FreeValue(ctx, s->value);
                s->value = c;
            }
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
                STEP_GOTO(s->hdr.stage, TS_READ, &s->phase, NULL);
                cb_result = JS_UNDEFINED;
                goto again;   /* a LOOP, never a call: this machine may not recurse into itself */
            }
            return JS_STEP_DONE;
        }
        if (s->done == 1) t->reading = 0;
        /* CLOSE and ERROR both settle the tee's own cancel promise, so a branch cancelled afterwards is not
           left waiting on a source that is already finished. */
        STEP_GOTO(s->hdr.stage, TS_RESOLVE_CANCEL, &s->phase, NULL);
    }

    if (s->hdr.stage == TS_RESOLVE_CANCEL) {
        JSValueConst undef = JS_UNDEFINED;
        if (t->canceled[0] && t->canceled[1]) { JS_FreeValue(ctx, cb_result); return JS_STEP_DONE; }
        r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), t->cancel_funcs[0], JS_UNDEFINED, 1, &undef, cb_result, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, out);
        return JS_STEP_DONE;
    }

    if (s->hdr.stage == TS_CANCEL_SOURCE) {
        JSValueConst arg = s->value;
        {
            JSValue fn = rs_fn(ctx, RSF_CANCEL);
            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), fn, t->reader, 1, &arg, cb_result, &out,
                              out_cb, out_argc);
            JS_FreeValue(ctx, fn);
        }
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, s->value);
        s->value = out;
        cb_result = JS_UNDEFINED;
        /* TWO CALLS, TWO STAGES. One `phase` byte serves one call at a time: writing the two in a row under a
           single byte made the resume re-enter the FIRST call with the SECOND's phase, which issued the second
           call again forever — a livelock the corpus found as a killed process, not as a failure. */
        STEP_GOTO(s->hdr.stage, TS_CANCEL_ADOPT, &s->phase, NULL);
    }

    DCHECK(s->hdr.stage == TS_CANCEL_ADOPT, "a tee machine resumed in a stage it never parks in");
    {
        /* §4.2 resolves cancelPromise WITH the source's cancel result, so the branches' cancel promises adopt
           it rather than settling ahead of it. */
        JSValueConst arg = s->value;
        r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), t->cancel_funcs[0], JS_UNDEFINED, 1, &arg, cb_result, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, out);
        return JS_STEP_DONE;
    }
}

#define TEE_DEF(i) { sizeof(JSTeeState), js_tee_step, js_tee_fini, (i), \
                     .algorithm = "Streams §4.9.1 ReadableStreamDefaultTee's pull, cancel and read reactions", \
                     .steps = TS_STEPS, \
                     .catches_abrupt = 1, .visit = js_tee_visit }
static const JSTrampStepDef js_tee_defs[TEE_N] = {
    TEE_DEF(TEE_PULL), TEE_DEF(TEE_CANCEL1), TEE_DEF(TEE_CANCEL2),
    TEE_DEF(TEE_READ_OK), TEE_DEF(TEE_READ_ERR), TEE_DEF(TEE_CLOSED_ERR),
};
#undef TEE_DEF

/* §4.2's `tee()`. A MACHINE because it acquires a reader, and §4.9.3's GenericInitialize settles `closed` at
   once on a stream that has already finished. */
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

/* A BRANCH: a stream and a controller whose pull and cancel are the tee's, not a page's. */
static int tee_branch(JSContext *ctx, TeeData *t, JSValueConst tee_v, int i)
{
    JSValueConst data[1];
    ControllerData *c;
    StreamData *d;

    t->branch[i] = readable_stream_empty(ctx);
    if (JS_IsException(t->branch[i])) return -1;
    d = rs_stream_data(t->branch[i]);
    t->ctrl[i] = JS_DupValue(ctx, d->controller);
    c = ctrl_of(t->ctrl[i]);
    data[0] = tee_v;
    c->pull_fn = JS_NewStepClosure(ctx, g_tee_stepids[TEE_PULL], 0, 1, data);
    if (JS_IsException(c->pull_fn)) { c->pull_fn = JS_UNDEFINED; return -1; }
    c->cancel_fn = JS_NewStepClosure(ctx, g_tee_stepids[i ? TEE_CANCEL2 : TEE_CANCEL1], 1, 1, data);
    if (JS_IsException(c->cancel_fn)) { c->cancel_fn = JS_UNDEFINED; return -1; }
    return 0;
}

/* `clone2` is §4.9.1's cloneForBranch2 — false for §4.2's `tee()`, true for Fetch's clone. It is a PARAMETER
   rather than a magic because an IdlStepBody is not handed one, and it is one body rather than two machines
   because the two differ by a single step inside one algorithm. It is read only at stage 0, where the record
   is built, so a resume never re-decides it. */
/* WHERE THIS MACHINE RESTS. §4.2's `tee()` is one step over §4.9.1's ReadableStreamDefaultTee, whose first
   step is acquiring the reader — a call — and whose last is the start promise's two reactions. */
#define TC_STAGES(X) \
    X(TC_BRAND = IDL_STEP_FIRST, \
      "Streams §4.9.1 ReadableStreamDefaultTee steps 1-2 (the stream, and the record the five algorithms " \
      "share)") \
    X(TC_READER, "Streams §4.9.1 ReadableStreamDefaultTee step 3 (AcquireReadableStreamDefaultReader)") \
    X(TC_BUILD, "Streams §4.9.1 ReadableStreamDefaultTee steps 4-17 (the two branches, built with " \
                "CreateReadableStream rather than with the page's `ReadableStream`)") \
    X(TC_STARTED, "Streams §4.9.1 ReadableStreamDefaultTee step 18 (the reader's `closed` reactions and the " \
                  "two branches, returned as a pair)")
enum { TC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TC_STEPS[] = { TC_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int tee_call_run(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc, bool clone2)
{
    JSTeeCallState *s = st;
    TeeData *t;
    JSValue obj, arr;
    int r, i;

    (void)argc; (void)argv;
    if (hdr->stage == TC_BRAND) {
        StreamData *sd;
        stream_work_start(&s->w);
        s->tee = s->reader = JS_UNDEFINED;
        STEP_GOTO(hdr->stage, TC_READER, &s->w.phase, NULL);
        sd = rs_stream_data(hdr->this_val);
        if (!sd) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "not a ReadableStream");
            return -1;
        }
        if (readable_byte_ctrl_is(sd->controller))
            DFAIL("`tee()` on a BYTE stream reached §4.9.1's ReadableStreamDefaultTee — §4.2 performs "
                  "ReadableByteStreamTee for one, whose branches are BYTE streams of their own and whose "
                  "reader switches between default and BYOB as the branches are read: build it beside this "
                  "one in readable_byte_stream.c");
    }
    if (hdr->stage == TC_READER) {
        {
            JSValue fn = rs_fn(ctx, RSF_GET_READER);
            r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), fn, hdr->this_val, 0, NULL,
                              cb_result, &s->reader, out_cb, out_argc);
            JS_FreeValue(ctx, fn);
        }
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
        t->clone_for_branch2 = (uint8_t)clone2;
        s->tee = obj;                      /* attached before anything that can fail, as everywhere here */
        t->reader = JS_DupValue(ctx, s->reader);
        t->cancel_promise = JS_NewPromiseCapability(ctx, t->cancel_funcs);
        if (JS_IsException(t->cancel_promise)) return -1;
        for (i = 0; i < 2; i++)
            if (tee_branch(ctx, t, s->tee, i) < 0) return -1;
        /* §4.2 step 18: the source's `closed` REJECTING is what errors both branches. */
        {
            JSValueConst data[1];
            ReaderData *rd = rs_reader_data(s->reader);
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
        STEP_GOTO(hdr->stage, TC_BUILD, &s->w.phase, NULL);
    }
    if (hdr->stage == TC_BUILD) {
        r = stream_promise_of_run(ctx, &s->w, 0, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        STEP_GOTO(hdr->stage, TC_STARTED, &s->w.phase, NULL);
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

static int js_tee_call_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    return tee_call_run(ctx, hdr, st, argc, argv, cb_result, presult, out_cb, out_argc, false);
}

static int js_tee_clone_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                             JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    return tee_call_run(ctx, hdr, st, argc, argv, cb_result, presult, out_cb, out_argc, true);
}

static const IdlStepDecl js_tee_call_decl = {
    js_tee_call_step, sizeof(JSTeeCallState), js_tee_call_visit, NULL,
    "Streams §4.2 tee(), through §4.9.1 ReadableStreamDefaultTee", TC_STEPS
};
/* THE SAME STATE AND THE SAME VISIT — one struct, one ownership contract, which is what the step-visits gate
   requires and what makes these two declarations of one algorithm rather than two algorithms. */
static const IdlStepDecl js_tee_clone_decl = {
    js_tee_clone_step, sizeof(JSTeeCallState), js_tee_call_visit, NULL,
    "Fetch §2.2.4 \"Bodies\" clone a body, through §4.9.1 ReadableStreamDefaultTee with cloneForBranch2 set", TC_STEPS
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

/* WHERE THIS MACHINE RESTS, AS §4.9.1 NUMBERS IT. ReadableStreamFromIterable's pullAlgorithm is one
   `nextMethod` call and its cancelAlgorithm is one `return` call, and everything after each is a reaction. */
#define FS_STAGES(X) \
    X(FS_START, "Streams §4.9.1 ReadableStreamFromIterable's pullAlgorithm step 1 / cancelAlgorithm steps " \
                "1-4 (which algorithm this entry is, and the `return` method it must look for)") \
    X(FS_CALL, "Web IDL §3.2.22.1 get the next value of an async iterator step 1 (IteratorNext — calling the " \
               "iterator's `next`) / close an async iterator step 6 (calling its `return`)") \
    X(FS_RESOLVE, "Web IDL §3.2.22.1 get the next value of an async iterator step 3 (a promise resolved with " \
                  "what `next` answered)") \
    X(FS_REJECT, "Web IDL §3.2.22.1 get the next value of an async iterator step 2 (a rejected promise for " \
                 "what the call threw)") \
    X(FS_REACT, "Web IDL §3.2.22.1 get the next value of an async iterator step 4 / close an async iterator " \
                "step 9 (reacting to that promise)") \
    X(FS_READ_DONE, "Web IDL §3.2.22.1 get the next value of an async iterator step 4.2 (IteratorComplete — " \
                    "Get(iterResult, \"done\"))") \
    X(FS_READ_VALUE, "Web IDL §3.2.22.1 get the next value of an async iterator step 4.4.1 (IteratorValue — " \
                     "Get(iterResult, \"value\"))") \
    X(FS_FEED, "Streams §4.9.1 ReadableStreamFromIterable steps 4.2.1.1-4.2.1.2 (close the controller on end " \
               "of iteration, or enqueue the value)")
enum { FS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FS_STEPS[] = { FS_STAGES(JS_STEP_STAGE_LABEL) NULL };

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


typedef struct {
    JSStepHdr hdr;
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
    (void)ctx;
    if (take_result) s->result = JS_UNDEFINED;
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

    if (s->hdr.stage == FS_START) {
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
            STEP_GOTO(s->hdr.stage, FS_READ_DONE, &s->phase, &s->hdr.get_phase, NULL);
        } else {
            s->value = JS_DupValue(ctx, s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED);
            STEP_GOTO(s->hdr.stage, FS_CALL, &s->phase, &s->hdr.get_phase, NULL);
        }
    }

    f = JS_GetOpaque(s->from, g_from_class);
    DCHECK(f != NULL, "a §4.2 `from` machine captured something that is not its record");

    if (s->hdr.stage == FS_CALL) {
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
                    STEP_GOTO(s->hdr.stage, FS_REJECT, &s->phase, &s->hdr.get_phase, NULL);
                    goto settle_promise;
                }
                JS_FreeValue(ctx, s->result);
                s->result = m;   /* held here only until the call is issued */
            }
            {
                JSValueConst arg = s->value;
                r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), s->result, f->iterator, 1, &arg, cb_result, &out,
                                  out_cb, out_argc);
            }
        } else {
            DCHECK(op == FROM_PULL, "a §4.2 `from` machine ran with an operation it does not have");
            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), f->next_fn, f->iterator, 0, NULL, cb_result, &out,
                              out_cb, out_argc);
        }
        if (r > 0) return r;
        if (JS_IsException(out)) {
            /* Web IDL §3.2.22.1 Iterating async sequences, which is what §4.2's `async_sequence<any>` runs:
               "If nextResult is an abrupt completion, return a promise rejected with nextResult.[[Value]]."
               — the algorithm answers a promise either way and never propagates, which is what makes a
               synchronously-throwing `next` error the stream through the same pull rejection a rejecting one
               takes. */
            JS_FreeValue(ctx, s->value);
            s->value = JS_GetException(ctx);
            STEP_GOTO(s->hdr.stage, FS_REJECT, &s->phase, &s->hdr.get_phase, NULL);
        } else {
            JS_FreeValue(ctx, s->value);
            s->value = out;
            STEP_GOTO(s->hdr.stage, FS_RESOLVE, &s->phase, &s->hdr.get_phase, NULL);
        }
        cb_result = JS_UNDEFINED;
    }

settle_promise:
    if (s->hdr.stage == FS_RESOLVE || s->hdr.stage == FS_REJECT) {
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
            s->chain = funcs[s->hdr.stage == FS_REJECT];
            JS_FreeValue(ctx, funcs[s->hdr.stage == FS_REJECT ? 0 : 1]);
        }
        r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), s->chain, JS_UNDEFINED, 1, &arg, cb_result, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, out);
        JS_FreeValue(ctx, s->chain);
        s->chain = JS_UNDEFINED;
        cb_result = JS_UNDEFINED;
        STEP_GOTO(s->hdr.stage, FS_REACT, &s->phase, &s->hdr.get_phase, NULL);
    }

    if (s->hdr.stage == FS_REACT) {
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

    if (s->hdr.stage == FS_READ_DONE) {
        atom = JS_NewAtom(ctx, "done");
        r = step_getprop_run(ctx, &s->hdr, s->value, atom, cb_result, &out, out_cb, out_argc);
        JS_FreeAtom(ctx, atom);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        s->done = (uint8_t)JS_ToBool(ctx, out);
        JS_FreeValue(ctx, out);
        STEP_GOTO(s->hdr.stage, s->done ? FS_FEED : FS_READ_VALUE, &s->phase, &s->hdr.get_phase, NULL);
    }
    if (s->hdr.stage == FS_READ_VALUE) {
        JSValue v;
        atom = JS_NewAtom(ctx, "value");
        r = step_getprop_run(ctx, &s->hdr, s->value, atom, cb_result, &v, out_cb, out_argc);
        JS_FreeAtom(ctx, atom);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        JS_FreeValue(ctx, s->value);
        s->value = v;
        STEP_GOTO(s->hdr.stage, FS_FEED, &s->phase, &s->hdr.get_phase, NULL);
    }

    DCHECK(s->hdr.stage == FS_FEED, "a §4.2 `from` machine resumed in a stage it never parks in");
    {
        JSValueConst arg = s->value;
        {
            JSValue fn = rs_ctrl_fn(ctx, s->done ? CF_CLOSE : CF_ENQUEUE);
            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), fn, f->controller,
                              s->done ? 0 : 1, &arg, cb_result, &out, out_cb, out_argc);
            JS_FreeValue(ctx, fn);
        }
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, out);
    }
    return JS_STEP_DONE;
}

#define FROM_DEF(i) { sizeof(JSFromState), js_from_step, js_from_fini, (i), \
                      .catches_abrupt = 1, .visit = js_from_visit, \
                      .algorithm = "Streams §4.9.1 ReadableStreamFromIterable's pull and cancel algorithms", \
                      .steps = FS_STEPS }
static const JSTrampStepDef js_from_defs[FROM_N] = {
    FROM_DEF(FROM_PULL), FROM_DEF(FROM_CANCEL), FROM_DEF(FROM_NEXT_OK), FROM_DEF(FROM_RET_OK),
};
#undef FROM_DEF

/* `static ReadableStream from(any asyncIterable)`. A MACHINE for GetIterator(obj, ASYNC): three of its four
   steps are the page's code. */
/* WHERE THIS MACHINE RESTS. §4.2's `from` is one step over §4.9.1's ReadableStreamFromIterable, whose first
   step is GetIterator(asyncIterable, ASYNC) — and three of that operation's four steps are the page's code. */
#define FC_STAGES(X) \
    X(FC_START = IDL_STEP_FIRST, \
      "ECMA-262 7.4.2 GetIterator step 1.a (GetMethod(obj, @@asyncIterator))") \
    X(FC_ASYNC_CALL, "ECMA-262 7.4.2 GetIterator step 3 (Call(method, obj) for the async iterator)") \
    X(FC_SYNC_GET, "ECMA-262 7.4.2 GetIterator step 1.b (GetMethod(obj, @@iterator), when there is no async " \
                   "one)") \
    X(FC_SYNC_CALL, "ECMA-262 7.4.2 GetIterator step 3 (Call(method, obj) for the sync iterator, then " \
                    "27.1.4.1 CreateAsyncFromSyncIterator over it)") \
    X(FC_NEXT, "ECMA-262 7.4.3 GetIteratorDirect step 1 (Get(iterator, \"next\"))") \
    X(FC_BUILD, "Streams §4.9.1 ReadableStreamFromIterable steps 2-4 (the stream, with the iterator's `next` " \
                "and `return` as its pull and cancel algorithms)") \
    X(FC_STARTED, "Streams §4.9.1 ReadableStreamFromIterable step 4's startAlgorithm (the start promise's " \
                  "reactions, attached before the stream is answered)")
enum { FC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FC_STEPS[] = { FC_STAGES(JS_STEP_STAGE_LABEL) NULL };

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

static int js_from_call_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                             JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSFromCallState *s = st;
    JSValueConst src = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue out;
    int r;

    if (hdr->stage == FC_START) {
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
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        s->is_sync = JS_IsUndefined(s->method) || JS_IsNull(s->method);
        STEP_GOTO(hdr->stage, s->is_sync ? FC_SYNC_GET : FC_ASYNC_CALL, &s->w.phase, &hdr->get_phase, NULL);
    }
    if (hdr->stage == FC_SYNC_GET) {
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
        STEP_GOTO(hdr->stage, FC_SYNC_CALL, &s->w.phase, &hdr->get_phase, NULL);
    }
    if (hdr->stage == FC_ASYNC_CALL || hdr->stage == FC_SYNC_CALL) {
        if (!JS_IsFunction(ctx, s->method)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "an iterable's iterator method is not callable");
            return -1;
        }
        r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->method, src, 0, NULL, cb_result, &s->iterator,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(s->iterator)) return -1;
        cb_result = JS_UNDEFINED;
        if (!JS_IsObject(s->iterator)) {
            JS_ThrowTypeError(ctx, "an iterator method answered with something that is not an object");
            return -1;
        }
        STEP_GOTO(hdr->stage, FC_NEXT, &s->w.phase, &hdr->get_phase, NULL);
    }
    if (hdr->stage == FC_NEXT) {
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
        STEP_GOTO(hdr->stage, FC_BUILD, &s->w.phase, &hdr->get_phase, NULL);
    }
    if (hdr->stage == FC_BUILD) {
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
        d = rs_stream_data(s->stream);
        c = ctrl_of(d->controller);
        f->controller = JS_DupValue(ctx, d->controller);
        c->hwm = 0;                        /* §4.2: `from` pulls only when asked */
        data[0] = s->from;
        c->pull_fn = JS_NewStepClosure(ctx, g_from_stepids[FROM_PULL], 0, 1, data);
        if (JS_IsException(c->pull_fn)) { c->pull_fn = JS_UNDEFINED; return -1; }
        c->cancel_fn = JS_NewStepClosure(ctx, g_from_stepids[FROM_CANCEL], 1, 1, data);
        if (JS_IsException(c->cancel_fn)) { c->cancel_fn = JS_UNDEFINED; return -1; }
        s->w.value = JS_UNDEFINED;
        STEP_GOTO(hdr->stage, FC_STARTED, &s->w.phase, &hdr->get_phase, NULL);
    }
    if (hdr->stage == FC_STARTED) {
        r = stream_promise_of_run(ctx, &s->w, 0, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, cb_result);
    if (ctrl_react(ctx, s->w.func, rs_stream_data(s->stream)->controller, RXN_START_OK, RXN_START_ERR) < 0)
        return -1;
    *presult = s->stream;
    s->stream = JS_UNDEFINED;
    return 0;
}

static const IdlStepDecl js_from_call_decl = {
    js_from_call_step, sizeof(JSFromCallState), js_from_call_visit, NULL,
    "Streams §4.2 from(asyncIterable), through §4.9.1 ReadableStreamFromIterable", FC_STEPS
};

/* ---- FETCH §2.2.4 "BODIES" — "FULLY READ" ---------------------------------------------------------------------------
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

/* APPEND A CHUNK'S BYTES — Streams §9.1.2 Reading's `read-loop`, whose chunk steps are exactly two lines: "If
   chunk is not a Uint8Array object, call failureSteps with a TypeError and abort these steps" and "Append the
   bytes represented by chunk to bytes". Returns -1 with a throw live.
 *
 * THE TWO LINES ARE INDEPENDENT AND THIS FUNCTION USED TO ANSWER BOTH WITH ONE READ, which got each of them
 * wrong in the opposite direction. The refusal was JS_GetArrayBufferView's, so it admitted every byte VIEW —
 * a DataView or an Int32Array a page enqueued was accepted where §9.1.2 names %Uint8Array% outright. And that
 * same read is how the appended bytes were reached, so a DETACHED Uint8Array — which IS a Uint8Array, and
 * whose §3.2.26 window is simply empty — was refused BY THE TYPE TEST and reported as "not a byte view": a
 * diagnosis that is false about the value and sends the reader to the page's chunk type.
 * ONE READ CANNOT ANSWER BOTH BECAUSE THEY ASK ABOUT DIFFERENT THINGS: the first is a BRAND question about the
 * object, the second is a question about the buffer under it. So the brand is asked as §9.1.2 states it, and
 * the detach is asked with the predicate Web IDL §3.2.26 Buffer source types' step 7 is stated over — every
 * question this site needs answered it asks itself, for the reason the DCHECK below gives. */
static int drain_append(JSContext *ctx, DrainData *dr, JSValueConst chunk)
{
    size_t off = 0, n = 0, whole = 0;
    JSValue buf;
    uint8_t *base;

    if (JS_GetTypedArrayType(chunk) != JS_TYPED_ARRAY_UINT8) {
        JS_ThrowTypeError(ctx, "a body stream answered with a chunk that is not a Uint8Array");
        return -1;
    }
    /* §3.2.26 STEP 7: "If IsDetachedBuffer(jsArrayBuffer) is true, then return the empty byte sequence." The
       chunk is still a Uint8Array, so §9.1.2's TypeError is not this value's answer; its window is empty, so
       "append the bytes represented by chunk" appends none and the read-loop goes on to the next chunk. */
    if (JS_IsDetachedBufferSource(chunk))
        return 0;
    buf = JS_GetArrayBufferView(ctx, chunk, &off, &n);
    DCHECK(!JS_IsException(buf),
           "a body stream's Uint8Array chunk is out of bounds with a buffer that is not detached — the brand "
           "and the detach are both answered above, so an out-of-bounds view has grown a cause this site does "
           "not know about (a length-tracking view over a shrunk resizable buffer is not one: §10.4.5.12 "
           "TypedArrayByteLength tracks it)");
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx, "a body stream answered with a chunk whose window is outside its own buffer");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &whole, buf);
    if (!base) { JS_FreeValue(ctx, buf); return -1; }
    /* THE WINDOW AND THE SIZE ARE ONE FACT ABOUT ONE ALLOCATION, asserted here because the copy below reads
       `n` bytes from `base + off`. THIS SITE HAS NO CONVERSION AHEAD OF IT: the chunk is whatever a page's
       own ReadableStream enqueued, so Web IDL §3.2.26 Buffer source types never ran on it and the only thing
       keeping a length-tracking view honest is that JS_GetArrayBufferView derives its length per ECMAScript
       §10.4.5.12 TypedArrayByteLength. That is exactly why the derivation had to be fixed in the engine and
       not only at the conversion — the conversion cannot see this chunk at all. */
    DCHECK(off <= whole && n <= whole - off,
           "a body stream's chunk reported a window outside its own buffer — JS_GetArrayBufferView and "
           "JS_GetArrayBuffer disagree about one allocation, and the copy below trusts the first");
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

/* WHERE THIS MACHINE RESTS. It is Fetch §2.2.4 "Bodies"'s "fully read a body" step 5 — "read all bytes from reader" —
   which the Streams standard states as a chain of reads, so each entry is one link of that chain. */
#define DS_STAGES(X) \
    X(DS_START, "Fetch §2.2.4 \"Bodies\" fully read a body step 5 → Streams read all bytes (what the previous read " \
                "answered: another chunk, the close, or an error)") \
    X(DS_READ, "Fetch §2.2.4 fully read a body step 5 (the next read through the reader)") \
    X(DS_RELEASE, "Fetch §2.2.4 fully read a body step 5 (releasing the reader once the body is whole)") \
    X(DS_SETTLE, "Fetch §5.3 \"Body mixin\" consume body step 4 (successSteps: convertBytesToJSValue over the collected " \
                 "bytes, then resolve the promise)")
enum { DS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DS_STEPS[] = { DS_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;
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

    if (s->hdr.stage == DS_START) {
        int k;
        s->drain = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0));
        s->value = JS_UNDEFINED;
        for (k = 0; k < 3; k++) s->cb[k] = JS_UNDEFINED;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        dr = JS_GetOpaque(s->drain, g_drain_class);
        DCHECK(dr != NULL, "a drain reaction captured something that is not a drain record");
        if (s->hdr.arg == DRAIN_ERR) {
            /* The stream errored. §2.2.4 rejects the body's promise with the stream's reason, and the reader is
               released with it — there is nothing left to read. */
            s->reject = 1;
            s->value = JS_DupValue(ctx, s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED);
            STEP_GOTO(s->hdr.stage, DS_RELEASE, &s->phase, NULL);
        } else {
            JSValue done_v = JS_GetPropertyStr(ctx, s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED, "done");
            int done = JS_ToBool(ctx, done_v);
            JS_FreeValue(ctx, done_v);
            if (done) {
                STEP_GOTO(s->hdr.stage, DS_RELEASE, &s->phase, NULL);
            } else {
                JSValue chunk = JS_GetPropertyStr(ctx, s->hdr.argv[0], "value");
                int bad = drain_append(ctx, dr, chunk) < 0;
                JS_FreeValue(ctx, chunk);
                if (bad) {
                    s->reject = 1;
                    s->value = JS_GetException(ctx);
                    STEP_GOTO(s->hdr.stage, DS_RELEASE, &s->phase, NULL);
                } else {
                    STEP_GOTO(s->hdr.stage, DS_READ, &s->phase, NULL);
                }
            }
        }
    }

    dr = JS_GetOpaque(s->drain, g_drain_class);
    DCHECK(dr != NULL, "a drain machine's record stopped being one");

    if (s->hdr.stage == DS_READ) {
        {
            JSValue fn = rs_fn(ctx, RSF_READ);
            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), fn, dr->reader, 0, NULL, cb_result, &out,
                              out_cb, out_argc);
            JS_FreeValue(ctx, fn);
        }
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        r = drain_react(ctx, out, s->drain);
        JS_FreeValue(ctx, out);
        return r < 0 ? JS_STEP_ABRUPT : JS_STEP_DONE;
    }

    if (s->hdr.stage == DS_RELEASE) {
        {
            JSValue fn = rs_fn(ctx, RSF_RELEASE);
            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), fn, dr->reader, 0, NULL, cb_result, &out,
                              out_cb, out_argc);
            JS_FreeValue(ctx, fn);
        }
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
                /* §2.2.4: an abrupt completion INSIDE the read is what the promise rejects with — which is how
                   `json()`'s SyntaxError reaches the page's `.catch` rather than the call site. */
                s->value = JS_GetException(ctx);
                s->reject = 1;
            }
        }
        STEP_GOTO(s->hdr.stage, DS_SETTLE, &s->phase, NULL);
    }

    DCHECK(s->hdr.stage == DS_SETTLE, "a drain machine resumed in a stage it never parks in");
    {
        JSValueConst arg = s->value;
        r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), dr->funcs[s->reject], JS_UNDEFINED, 1, &arg, cb_result, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, out);
    }
    return JS_STEP_DONE;
}

#define DRAIN_DEF(i) { sizeof(JSDrainState), js_drain_step, NULL, (i), \
                       .catches_abrupt = 1, .visit = js_drain_visit, \
                       .algorithm = "Fetch §2.2.4 fully read a body step 5 (read all bytes from a reader)", \
                       .steps = DS_STEPS }
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

/* §4.9.4's CreateReadableStream — a stream whose ALGORITHMS are the caller's function objects rather than a
   page's underlying source. It is the operation §6's TransformStream is built out of: a transform stream's
   readable half has no source object at all, only closures over the transform stream itself.
   `pull_fn` is called with the controller and `cancel_fn` with the reason, both with `this` = the source, which
   for this operation is undefined; `size_fn` may be undefined for the implicit one-per-chunk strategy. All are
   BORROWED. THE START ALGORITHM IS NOT HERE — see readable_stream_start, for the reason §5's twin gives. */
JSValue readable_stream_create(JSContext *ctx, JSValueConst pull_fn, JSValueConst cancel_fn,
                               double hwm, JSValueConst size_fn)
{
    JSValue obj = readable_stream_empty(ctx);
    ControllerData *c;

    if (JS_IsException(obj)) return obj;
    c = ctrl_of(rs_stream_data(obj)->controller);
    DCHECK(c != NULL, "an empty ReadableStream came back without its controller");
    c->pull_fn = JS_DupValue(ctx, pull_fn);
    c->cancel_fn = JS_DupValue(ctx, cancel_fn);
    c->size_fn = JS_DupValue(ctx, size_fn);
    c->hwm = hwm;
    return obj;
}

int readable_stream_start(JSContext *ctx, JSValueConst stream, JSValueConst start_promise)
{
    StreamData *d = rs_stream_data(stream);
    DCHECK(d != NULL, "a §4 start was reported for something that is not a ReadableStream");
    return ctrl_react(ctx, start_promise, d->controller, RXN_START_OK, RXN_START_ERR);
}

JSValue readable_stream_op(JSContext *ctx, ReadableStreamOp which)
{
    switch (which) {
    case RS_OP_GET_READER: return rs_fn(ctx, RSF_GET_READER);
    case RS_OP_READ:       return rs_fn(ctx, RSF_READ);
    case RS_OP_RELEASE:    return rs_fn(ctx, RSF_RELEASE);
    case RS_OP_TEE:        return rs_fn(ctx, RSF_TEE);
    case RS_OP_TEE_CLONE:  return rs_fn(ctx, RSF_TEE_CLONE);
    default:
        DCHECK(which == RS_OP_CANCEL, "a stream operation was asked for by a name this component does not map");
        return rs_fn(ctx, RSF_CANCEL);
    }
}

JSValue readable_stream_ctrl_op(JSContext *ctx, ReadableControllerOp which)
{
    return rs_ctrl_fn(ctx, which);
}

JSValueConst readable_stream_controller(JSValueConst stream)
{
    StreamData *d = rs_stream_data(stream);
    DCHECK(d != NULL, "the controller of something that is not a ReadableStream was asked for");
    return d->controller;
}

bool readable_ctrl_can_close_or_enqueue(JSContext *ctx, JSValueConst ctrl)
{
    ControllerData *c = ctrl_of(ctrl);
    (void)ctx;
    if (!c) return false;
    return ctrl_can_close_or_enqueue(c, rs_stream_data(c->stream));
}

bool readable_ctrl_has_backpressure(JSContext *ctx, JSValueConst ctrl)
{
    ControllerData *c = ctrl_of(ctrl);
    /* §4.5: HasBackpressure is the NEGATION of ShouldCallPull — the readable half wants no more exactly when
       it would not pull. §6.3 reads it after every enqueue to decide whether the writable half must push
       back, which is the only channel between the two halves of a transform stream. */
    if (!c) return false;
    return !ctrl_should_pull(ctx, c, rs_stream_data(c->stream));
}

bool readable_stream_query(JSValueConst v, ReadableStreamState *pstate, bool *plocked)
{
    StreamData *d = rs_stream_data(v);

    if (!d) return false;
    if (pstate) *pstate = (ReadableStreamState)d->state;
    if (plocked) *plocked = !JS_IsUndefined(d->reader);
    return true;
}

JSValue readable_stream_stored_error(JSContext *ctx, JSValueConst v)
{
    StreamData *d = rs_stream_data(v);
    return d ? JS_DupValue(ctx, d->stored_error) : JS_UNDEFINED;
}

JSValue readable_reader_closed(JSContext *ctx, JSValueConst reader)
{
    ReaderData *r = rs_reader_data(reader);
    DCHECK(r != NULL, "the closed promise of something that is not a reader was asked for");
    return JS_DupValue(ctx, r->closed);
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
/* WHERE THIS MACHINE RESTS, AS §4.2 NUMBERS IT. The constructor is nine steps and four of them can run the
   page's code: Web IDL §3.7.1's `prototype` read, each UnderlyingSource member read, `start` itself, and the
   PromiseResolve over what it returned. */
#define RSC_STAGES(X) \
    X(RSC_START = IDL_STEP_FIRST, \
      "Streams §4.2 new ReadableStream(underlyingSource, strategy) step 1 (the source is null when missing; " \
      "Web IDL §3.7.1's `new` requirement precedes it)") \
    X(RSC_PROTO, "Web IDL §3.7.1 (Get(newTarget, \"prototype\") — what makes " \
                 "`class S extends ReadableStream {}` produce an S)") \
    X(RSC_READ, "Streams §4.2 step 2 (converting underlyingSource to an UnderlyingSource: one [[Get]] per " \
                "member, in the order Web IDL §3.2.17 reads them)") \
    X(RSC_ALLOC, "Web IDL §3.2.4.8 (UnderlyingSource[\"autoAllocateChunkSize\"] is [EnforceRange] unsigned long " \
                 "long, so its ToNumber runs BEFORE `cancel` is even read)") \
    X(RSC_TYPE, "Streams §4.2 steps 4-5 (underlyingSourceDict[\"type\"]: \"bytes\" picks §4.7's byte " \
                "controller and anything else is a TypeError)") \
    X(RSC_BUILD, "Streams §4.2 steps 3 and 6-7 (InitializeReadableStream, ExtractHighWaterMark, " \
                 "ExtractSizeAlgorithm, and §4.9.4's/§4.9.5's SetUp…FromUnderlyingSource up to its start " \
                 "algorithm)") \
    X(RSC_CALL, "Streams §4.9.4 SetUpReadableStreamDefaultController step 9 (the start algorithm — the " \
                "source's own `start`, invoked with the controller)") \
    X(RSC_RESOLVE, "Streams §4.9.4 SetUpReadableStreamDefaultController step 10 (a promise resolved with " \
                   "startResult — 27.5.1.3 step 2.f's `then` read is the page's)") \
    X(RSC_THEN, "Streams §4.9.4 SetUpReadableStreamDefaultController steps 11-12 (the start promise's two " \
                "reactions are attached and the stream is the constructor's result)")
enum { RSC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RSC_STEPS[] = { RSC_STAGES(JS_STEP_STAGE_LABEL) NULL };
/* UnderlyingSource's members, in the order Web IDL reads them. */
enum { SRC_CHUNKSIZE = 0, SRC_CANCEL, SRC_PULL, SRC_START, SRC_TYPE, SRC_N };

typedef struct {
    StreamWork w;
    uint8_t  member;         /* which UnderlyingSource member the read loop is on */
    uint8_t  bytes;          /* underlyingSourceDict["type"] is "bytes" */
    uint64_t auto_alloc;     /* [[autoAllocateChunkSize]], 0 meaning the member was absent */
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

    if (hdr->stage == RSC_START) {
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
        s->bytes = 0;
        s->auto_alloc = 0;
        STEP_GOTO(hdr->stage, RSC_PROTO, &s->w.phase, &hdr->get_phase, NULL);
    }

    if (hdr->stage == RSC_PROTO) {
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
        STEP_GOTO(hdr->stage, JS_IsObject(source) ? RSC_READ : RSC_BUILD, &s->w.phase, &hdr->get_phase, NULL);
    }

    while (hdr->stage == RSC_READ || hdr->stage == RSC_ALLOC) {
        if (hdr->stage == RSC_READ) {
            JSAtom a;
            if (s->member >= SRC_N) { STEP_GOTO(hdr->stage, RSC_TYPE, &s->w.phase, &hdr->get_phase, NULL); break; }
            a = JS_NewAtom(ctx, SRC_NAMES[s->member]);
            r = step_getprop_run(ctx, hdr, source, a, cb_result, &s->src[s->member], out_cb, out_argc);
            JS_FreeAtom(ctx, a);
            if (r > 0) return r;
            if (r < 0) return -1;
            cb_result = JS_UNDEFINED;
            s->member++;
            /* Web IDL §3.2.17 converts each member AS IT IS READ, and this one's conversion is ToNumber — the page's
               `valueOf`, running before `cancel` is even read. */
            if (s->member == SRC_CANCEL && !JS_IsUndefined(s->src[SRC_CHUNKSIZE]))
                STEP_GOTO(hdr->stage, RSC_ALLOC, &s->w.phase, &hdr->get_phase, NULL);
            continue;
        }
        {
            double x = 0;
            r = step_todouble_run(ctx, hdr, s->src[SRC_CHUNKSIZE], cb_result, &x, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            cb_result = JS_UNDEFINED;
            /* [EnforceRange]: a fractional, negative, non-finite or too-large value is a TypeError, never a
               silent clamp. */
            if (!isfinite(x) || x != trunc(x) || x < 0 || x > 18446744073709551615.0) {
                JS_ThrowTypeError(ctx, "autoAllocateChunkSize is outside the range of an unsigned long long");
                return -1;
            }
            s->auto_alloc = (uint64_t)x;
            STEP_GOTO(hdr->stage, RSC_READ, &s->w.phase, &hdr->get_phase, NULL);
        }
    }

    if (hdr->stage == RSC_TYPE) {
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
            s->bytes = 1;
        }
        /* §4.9.5's SetUpReadableByteStreamControllerFromUnderlyingSource step 7. The member is read and
           converted for EVERY underlying source — it is part of the dictionary — and a default controller
           simply has nothing to do with it; only its value 0 is refused, and only where it is used. */
        if (s->bytes && !JS_IsUndefined(s->src[SRC_CHUNKSIZE]) && s->auto_alloc == 0) {
            JS_ThrowTypeError(ctx, "an underlying byte source's autoAllocateChunkSize must not be 0");
            return -1;
        }
        if (stream_callback_member(ctx, s->src[SRC_CANCEL], "source", "cancel") < 0) return -1;
        if (stream_callback_member(ctx, s->src[SRC_PULL], "source", "pull") < 0) return -1;
        if (stream_callback_member(ctx, s->src[SRC_START], "source", "start") < 0) return -1;
        cb_result = JS_UNDEFINED;
        STEP_GOTO(hdr->stage, RSC_BUILD, &s->w.phase, &hdr->get_phase, NULL);
    }

    if (hdr->stage == RSC_BUILD) {
        JSValueConst strategy = argc > 1 ? argv[1] : JS_UNDEFINED;
        ControllerData *c;
        JSValue hv;
        int absent;
        double h = 1;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* §4.2 steps 6 and 7: ExtractSizeAlgorithm, then ExtractHighWaterMark — whose DEFAULT is 1 for a
           default controller and 0 for a byte one, because a byte source is not asked to produce anything until
           somebody reads. Both read the strategy dictionary the IDL layer has ALREADY converted, which is why a
           throwing `get size` is seen before a throwing `get start`. */
        hv = idl_dict_get(ctx, strategy, "highWaterMark");
        absent = JS_IsUndefined(hv);
        if (!absent && JS_ToFloat64(ctx, &h, hv) < 0) { JS_FreeValue(ctx, hv); return -1; }
        JS_FreeValue(ctx, hv);
        if (!absent && (h != h || h < 0)) {
            /* §4.2's own check, not the type's: `unrestricted double` accepts NaN, and the STREAM is what
               rejects it. */
            JS_ThrowRangeError(ctx, "a queuing strategy's highWaterMark must not be negative or NaN");
            return -1;
        }
        {
            JSValue size_fn = idl_dict_get(ctx, strategy, "size");
            /* §4.2 step 4.1: a BYTE stream has no size algorithm at all — its queue is measured in bytes — so
               a strategy that supplies one is a RangeError rather than something to ignore. */
            if (s->bytes && !JS_IsUndefined(size_fn)) {
                JS_FreeValue(ctx, size_fn);
                JS_ThrowRangeError(ctx, "a byte stream's queuing strategy cannot have a size algorithm");
                return -1;
            }
            s->stream = readable_stream_create(ctx, s->src[SRC_PULL], s->src[SRC_CANCEL],
                                               absent ? (s->bytes ? 0 : 1) : h, size_fn);
            JS_FreeValue(ctx, size_fn);
        }
        if (JS_IsException(s->stream)) return -1;
        /* §3.7.1's prototype, applied to the object CreateReadableStream just built: the operation makes a
           ReadableStream and the CONSTRUCTOR decides which one a subclass gets. */
        if (JS_IsObject(s->proto) && JS_SetPrototype(ctx, s->stream, s->proto) < 0) return -1;
        if (s->bytes) {
            /* §4.9.5's SetUpReadableByteStreamController REPLACES the controller CreateReadableStream attached:
               a stream has exactly one, and which one it is decides what every §4.9.2 operation does. */
            if (readable_byte_ctrl_setup(ctx, s->stream, s->src[SRC_PULL], s->src[SRC_CANCEL], source,
                                         absent ? 0 : h, s->auto_alloc) < 0)
                return -1;
            s->controller = JS_DupValue(ctx, rs_stream_data(s->stream)->controller);
        } else {
            s->controller = JS_DupValue(ctx, rs_stream_data(s->stream)->controller);
            c = ctrl_of(s->controller);
            DCHECK(c != NULL, "CreateReadableStream answered a stream with no controller");
            /* The SOURCE is the receiver §4.9.4 invokes the page's methods on — CreateReadableStream has no
               source at all, so it is set here rather than inside the operation. */
            rc_set(ctx, c, &c->source, JS_DupValue(ctx, source));
        }
        s->start_fn = s->src[SRC_START];
        s->src[SRC_START] = JS_UNDEFINED;
        STEP_GOTO(hdr->stage, JS_IsFunction(ctx, s->start_fn) ? RSC_CALL : RSC_RESOLVE, &s->w.phase,
                  &hdr->get_phase, NULL);
    }

    if (hdr->stage == RSC_CALL) {
        JSValue res;
        arg = s->controller;
        r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->start_fn, s->source, 1, &arg,
                          cb_result, &res, out_cb, out_argc);
        if (r > 0) return r;
        JS_FreeValue(ctx, s->w.value);
        s->w.value = res;
        cb_result = JS_UNDEFINED;
        STEP_GOTO(hdr->stage, RSC_RESOLVE, &s->w.phase, &hdr->get_phase, NULL);
    }
    if (hdr->stage == RSC_RESOLVE) {
        r = stream_promise_of_run(ctx, &s->w, 0, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        STEP_GOTO(hdr->stage, RSC_THEN, &s->w.phase, &hdr->get_phase, NULL);
    }

    DCHECK(hdr->stage == RSC_THEN, "the ReadableStream constructor resumed in a stage it never parks in");
    JS_FreeValue(ctx, cb_result);
    /* The start promise's two reactions belong to whichever controller was set up — §4.9.4's step 11-12 or
       §4.9.5's step 15-16 — and the CONSTRUCTOR knows which one it built. */
    if ((s->bytes ? readable_byte_start(ctx, s->stream, s->w.func)
                  : readable_stream_start(ctx, s->stream, s->w.func)) < 0) return -1;
    *presult = s->stream;
    s->stream = JS_UNDEFINED;
    return 0;
}

static const IdlStepDecl js_rs_ctor_decl = {
    js_rs_ctor_step, sizeof(JSRsCtorState), js_rs_ctor_visit, NULL,
    "Streams §4.2 new ReadableStream(underlyingSource, strategy)", RSC_STEPS
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

    g_getreader_id = idl_method_id_step(ctx, ONE_DICT, 1, GET_READER_OPTIONS,
                                       (int)(sizeof GET_READER_OPTIONS / sizeof *GET_READER_OPTIONS),
                                       &js_get_reader_decl, GR_SELF);
    idl_optional_from(0);   /* §4.2: `getReader(optional ReadableStreamGetReaderOptions options = {})` */
    for (i = 0; i < 3; i++) {
        g_cancel_stepids[i] = JS_RegisterStepDef(rt, &js_cancel_defs[i]);
        CHECK(g_cancel_stepids[i] >= 0, "streams: no step id for cancel");
    }

    {
        JSClassDef cd = { "ReadableStreamDefaultController", .finalizer = ctrl_finalizer,
                          .gc_mark = ctrl_gc_mark };
        JS_NewClassID(rt, &g_ctrl_class);
        JS_NewClass(rt, g_ctrl_class, &cd);
        for (i = 0; i < 3; i++) {
            g_ctrl_stepids[i] = JS_RegisterStepDef(rt, &js_ctrl_defs[i]);
            CHECK(g_ctrl_stepids[i] >= 0, "streams: no step id for a controller member");
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

    for (i = 0; i < 2; i++) {
        g_release_stepids[i] = JS_RegisterStepDef(rt, &js_release_defs[i]);
        CHECK(g_release_stepids[i] >= 0, "streams: no step id for releaseLock");
    }
    g_read_stepid = JS_RegisterStepDef(rt, &js_read_def);
    CHECK(g_read_stepid >= 0, "streams: no step id for read");

    /* §4.7's controller and §4.8's BYOB request are their own component, and it declares itself here for the
       reason every other one does: a realm's list of intrinsics is built from the declarations, so a component
       nobody initialises is a component no realm has. */
    readable_byte_stream_init(ctx);
    g_byob_proto_slot = realm_value_declare(ctx, "ReadableStreamBYOBReader.prototype");

    /* §4.2.1's `async_iterable<any>(optional ReadableStreamIteratorOptions options = {})`. The DECLARATION is
       Web IDL's — the iterator class, its prototype, the three step machines behind `next` and `return`, and
       §3.7.10's members — and all this component declares is §4.2.5's three algorithms and the two reactions
       its read request is made of. */
    for (i = 0; i < RSI_RXN_N; i++) {
        g_rsi_stepids[i] = JS_RegisterStepDef(rt, &js_rs_iter_rxn_defs[i]);
        CHECK(g_rsi_stepids[i] >= 0, "streams: no step id for a §4.2.5 read request");
    }
    g_rs_iter_handle = idl_async_iter_declare(ctx, &RS_ITER_OPS);

    /* §4.2's tee. Its branches' pull and cancel are step closures, so they need the controller's members as
       function objects for the same reason §4.4 needs the reader's. */
    {
        JSClassDef td = { "ReadableStream tee", .finalizer = tee_finalizer, .gc_mark = tee_gc_mark };
        JS_NewClassID(rt, &g_tee_class);
        JS_NewClass(rt, g_tee_class, &td);
        for (i = 0; i < TEE_N; i++) {
            g_tee_stepids[i] = JS_RegisterStepDef(rt, &js_tee_defs[i]);
            CHECK(g_tee_stepids[i] >= 0, "streams: no step id for a tee operation");
        }
        g_tee_id = idl_method_id_step(ctx, NULL, 0, NULL, 0, &js_tee_call_decl, 0);
        /* §4.9.1 with cloneForBranch2, which is NOT a page-visible member — it is the operation Fetch's "clone
           a body" performs, so it is a function object this component hands out and nothing installs. */
        g_tee_clone_id = idl_method_id_step(ctx, NULL, 0, NULL, 0, &js_tee_clone_decl, 0);
    }

    /* Fetch §2.2.4 "Bodies"'s "fully read", whose record is a class for the reason every other one here is: it is a
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
    /* §4.4's and §4.5's constructors ARE getReader spelled the other two ways — SetUpReadableStream*Reader
       reached with the stream in an argument instead of in the receiver, which is all the magic says. */
    g_reader_ctor_stepid = idl_method_id_step(ctx, ONE_ANY, 1, NULL, 0, &js_get_reader_decl, GR_CTOR);
    g_byob_ctor_stepid = idl_method_id_step(ctx, ONE_ANY, 1, NULL, 0, &js_get_reader_decl, GR_CTOR_BYOB);

    /* §4.9.1's ReadableStreamPipeTo is its own component — it holds a reader on one stream and a writer on
       another, so it belongs to neither half — but `pipeTo` and `pipeThrough` are §4.2's MEMBERS, and this is
       the prototype they go on. The declaration is piping's; the placement is this interface's. */
    pipe_init(ctx);
    {
        static const char *const FN_NAME[RSF_N] = {
            "reader.read", "reader.releaseLock", "reader.cancel", "ReadableStream.getReader",
            "ReadableStream.tee", "§4.9.1 cloning tee",
        };
        static const char *const CTRL_NAME[RS_CTRL_N] = {
            "controller.enqueue", "controller.close", "controller.error",
        };
        for (i = 0; i < RSF_N; i++)   g_rs_fn_slot[i] = realm_value_declare(ctx, FN_NAME[i]);
        for (i = 0; i < RS_CTRL_N; i++) g_ctrl_fn_slot[i] = realm_value_declare(ctx, CTRL_NAME[i]);
    }
    realm_declare_intrinsic(readable_stream_install_protos);
    realm_declare_intrinsic(readable_byte_stream_install_protos);
}

/* §4's FOUR INTERFACE PROTOTYPE OBJECTS, FOR ONE REALM, and the abstract operations read off them while they
   are still the ones just installed. */
void readable_stream_install_protos(JSContext *ctx)
{
    static const char *const CTRL_NAMES[RS_CTRL_N] = { "enqueue", "close", "error" };
    JSValue stream_p, reader_p, ctrl_p, prev;
    int i;

    DCHECK(g_stream_class != 0, "a realm asked for ReadableStream.prototype before the class was declared");
    prev = JS_GetClassProto(ctx, g_stream_class);
    DCHECK(JS_IsNull(prev), "readable_stream_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);

    stream_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(stream_p), "ReadableStream.prototype could not be allocated");
    idl_interface_tag(ctx, stream_p, "ReadableStream");
    idl_install_accessor(ctx, stream_p, "locked", js_stream_locked, 0, -1);
    idl_install_method(ctx, stream_p, "getReader", g_getreader_id);
    idl_install_step_method(ctx, stream_p, "cancel", 0, g_cancel_stepids[CANCEL_ON_STREAM]);
    idl_install_method(ctx, stream_p, "tee", g_tee_id);
    /* §4.2.1's `async_iterable<any>` — §3.7.10's `values` and %Symbol.asyncIterator% (the SAME function
       object), §3.7.10.1's iterator and §3.7.10.2's prototype under this realm's %AsyncIteratorPrototype%. All
       of it Web IDL's, installed here because the two halves are installed together or not at all. */
    idl_async_iter_install(ctx, stream_p, g_rs_iter_handle);
    /* §4.9.1's ReadableStreamPipeTo is its own component — it holds a reader on one stream and a writer on
       another, so it belongs to neither half — but `pipeTo` and `pipeThrough` are §4.2's MEMBERS, and this is
       the prototype they go on. The declaration is piping's; the placement is this interface's. */
    pipe_install(ctx, stream_p);
    JS_SetClassProto(ctx, g_stream_class, stream_p);

    ctrl_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(ctrl_p), "the controller prototype could not be allocated");
    idl_interface_tag(ctx, ctrl_p, "ReadableStreamDefaultController");
    idl_install_accessor(ctx, ctrl_p, "desiredSize", js_ctrl_desired, 0, -1);
    for (i = 0; i < RS_CTRL_N; i++)
        JS_SetPropertyStr(ctx, ctrl_p, CTRL_NAMES[i],
                          JS_NewCFunction2(ctx, NULL, CTRL_NAMES[i], 1, JS_CFUNC_step, g_ctrl_stepids[i]));
    JS_SetClassProto(ctx, g_ctrl_class, ctrl_p);

    reader_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(reader_p), "ReadableStreamDefaultReader.prototype could not be allocated");
    idl_interface_tag(ctx, reader_p, "ReadableStreamDefaultReader");
    idl_install_accessor(ctx, reader_p, "closed", js_reader_closed, 0, -1);
    idl_install_step_method(ctx, reader_p, "releaseLock", 0, g_release_stepids[0]);
    idl_install_step_method(ctx, reader_p, "read", 0, g_read_stepid);
    idl_install_step_method(ctx, reader_p, "cancel", 0, g_cancel_stepids[CANCEL_ON_DEFAULT]);
    JS_SetClassProto(ctx, g_reader_class, reader_p);

    /* §4.5's ReadableStreamBYOBReader, whose THREE machine members — §4.5's own `read(view, options)` and
       `releaseLock()`, and §4.3's mixin `cancel()` — are the same three machines with the other brand, and
       whose `read` is §4.9.5's pull-into. It is the same CLASS as §4.4's reader — one
       record, one lock — so its prototype cannot be the class's, and lives in a per-realm value slot. */
    {
        JSValue byob_p = JS_NewObject(ctx);
        CHECK(!JS_IsException(byob_p), "ReadableStreamBYOBReader.prototype could not be allocated");
        idl_interface_tag(ctx, byob_p, "ReadableStreamBYOBReader");
        idl_install_accessor(ctx, byob_p, "closed", js_reader_closed, 1, -1);
        idl_install_step_method(ctx, byob_p, "releaseLock", 0, g_release_stepids[1]);
        idl_install_step_method(ctx, byob_p, "read", 1, readable_byob_read_stepid());
        idl_install_step_method(ctx, byob_p, "cancel", 0, g_cancel_stepids[CANCEL_ON_BYOB]);
        realm_value_set(ctx, g_byob_proto_slot, byob_p);
    }

    /* THE ABSTRACT OPERATIONS, read off the prototypes nothing of the page's has touched yet. */
    realm_value_set(ctx, g_rs_fn_slot[RSF_READ],       JS_GetPropertyStr(ctx, reader_p, "read"));
    realm_value_set(ctx, g_rs_fn_slot[RSF_RELEASE],    JS_GetPropertyStr(ctx, reader_p, "releaseLock"));
    realm_value_set(ctx, g_rs_fn_slot[RSF_CANCEL],     JS_GetPropertyStr(ctx, reader_p, "cancel"));
    realm_value_set(ctx, g_rs_fn_slot[RSF_GET_READER], JS_GetPropertyStr(ctx, stream_p, "getReader"));
    realm_value_set(ctx, g_rs_fn_slot[RSF_TEE],        JS_GetPropertyStr(ctx, stream_p, "tee"));
    realm_value_set(ctx, g_rs_fn_slot[RSF_TEE_CLONE],  idl_step_function(ctx, "tee", g_tee_clone_id));
    for (i = 0; i < RSF_N; i++) {
        JSValue fn = rs_fn(ctx, i);
        CHECK(JS_IsFunction(ctx, fn), "streams: an operation §4 performs was not installed before capture");
        JS_FreeValue(ctx, fn);
    }
    for (i = 0; i < RS_CTRL_N; i++) {
        JSValue fn = JS_GetPropertyStr(ctx, ctrl_p, CTRL_NAMES[i]);
        CHECK(JS_IsFunction(ctx, fn),
              "streams: a controller member the tee performs was not installed before it was captured");
        realm_value_set(ctx, g_ctrl_fn_slot[i], fn);
    }
}

void readable_stream_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;
    DCHECK(g_ctor_stepid >= 0, "ReadableStream was installed before its constructor was declared");
    ctor = idl_step_constructor(ctx, "ReadableStream", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the ReadableStream interface object could not be allocated");
    {
        JSValue proto = JS_GetClassProto(ctx, g_stream_class);
        DCHECK(!JS_IsNull(proto), "ReadableStream was installed into a realm with no proto build");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    /* §4.2's `from` is STATIC, so it lives on the interface object rather than the prototype. */
    idl_install_method(ctx, ctor, "from", g_from_ctor_stepid);
    idl_define_global_property_reference(ctx, global, "ReadableStream", ctor);

    ctor = idl_step_constructor(ctx, "ReadableStreamDefaultReader", g_reader_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the reader interface object could not be allocated");
    {
        JSValue proto = JS_GetClassProto(ctx, g_reader_class);
        DCHECK(!JS_IsNull(proto), "ReadableStreamDefaultReader was installed into a realm with no proto build");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    idl_define_global_property_reference(ctx, global, "ReadableStreamDefaultReader", ctor);

    ctor = idl_step_constructor(ctx, "ReadableStreamBYOBReader", g_byob_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the BYOB reader interface object could not be allocated");
    {
        JSValue proto = readable_byob_reader_proto(ctx);
        DCHECK(JS_IsObject(proto), "ReadableStreamBYOBReader was installed into a realm with no proto build");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    idl_define_global_property_reference(ctx, global, "ReadableStreamBYOBReader", ctor);

    ctor = JS_NewCFunction2(ctx, js_illegal_ctor, "ReadableStreamDefaultController", 0,
                            JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(ctor), "the controller interface object could not be allocated");
    {
        JSValue proto = JS_GetClassProto(ctx, g_ctrl_class);
        DCHECK(!JS_IsNull(proto), "ReadableStreamDefaultController was installed into a realm with no proto build");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }   /* .prototype and .constructor, both directions */
    idl_define_global_property_reference(ctx, global, "ReadableStreamDefaultController", ctor);

    /* §4.7's and §4.8's interface objects are their component's. */
    readable_byte_stream_install(ctx, global);
}

void readable_stream_free(JSContext *ctx)
{
    int i;
    if (!g_rs_rt) return;
    pipe_free(ctx);
    /* the prototypes and the captured operations are the REALMS' — released with their contexts */
    for (i = 0; i < RSI_RXN_N; i++) g_rsi_stepids[i] = -1;
    g_rs_iter_handle = -1;
    for (i = 0; i < TEE_N; i++) g_tee_stepids[i] = -1;
    for (i = 0; i < FROM_N; i++) g_from_stepids[i] = -1;
    for (i = 0; i < DRAIN_N; i++) g_drain_stepids[i] = -1;
    g_from_ctor_stepid = -1;
    g_rs_rt = NULL;
    g_ctor_stepid = g_reader_ctor_stepid = g_read_stepid = g_byob_ctor_stepid = -1;
    for (i = 0; i < 3; i++) g_cancel_stepids[i] = -1;
    for (i = 0; i < 2; i++) g_release_stepids[i] = -1;
    readable_byte_stream_free();
    for (i = 0; i < 3; i++) g_ctrl_stepids[i] = -1;
    for (i = 0; i < 4; i++) g_rxn_stepids[i] = -1;
    for (i = 0; i < 2; i++) g_fwd_stepids[i] = -1;
}
