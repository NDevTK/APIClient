/* ReadableByteStreamController — the Streams Standard §4.7, §4.8 and §4.9.5.
 *
 * WHY IT IS ITS OWN COMPONENT. §4.6's controller and this one are two implementations of ONE contract (§4.9.2's
 * three internal methods), and the standard writes them apart because the thing they disagree about is the
 * QUEUE: §4.6 holds the page's chunks and weighs each with the strategy's size algorithm, and this one holds
 * BYTE RANGES — a buffer, an offset and a length — whose total is a byte count and whose consumer may be a
 * reader that supplied the destination memory itself. A byte controller wearing §4.6's queue would be the wrong
 * implementation with the right name, so it does not have one.
 *
 * WHAT IS HERE: §4.7's controller with its byte queue and its list of PULL-INTO DESCRIPTORS, §4.8's BYOB
 * request, §4.5's `read(view, {min})` (the reader OBJECT is §4.3's mixin and lives with the reader class in
 * readable_stream.c, because a BYOB reader and a default reader are one record and one lock), and §4.9.5's
 * abstract operations.
 *
 * THE BUFFERS ARE TRANSFERRED, AND THAT IS THE OBSERVABLE BEHAVIOUR. §8.3's TransferArrayBuffer DETACHES what
 * it was given and answers a new buffer over the same bytes, and every path here performs it: an enqueued
 * chunk's buffer, the view a BYOB read supplied, the descriptor a respond fills. A page that reads its own view
 * after handing it to `read()` must see a detached buffer, and a stream that skipped the detach would be a
 * different stream — one whose consumer and producer can write the same memory at once.
 *
 * THE PULL-INTO DESCRIPTORS AND THE QUEUE ENTRIES ARE JS OBJECTS IN JS ARRAYS. They are state a FLOW queues
 * into, so each must park to the cold tier and resume, and each must fork per flow — which an Array gives for
 * nothing, because its mutations are property writes the COW delta already captures. A malloc'd list captured
 * as head/tail pointers would revert the pointers on a context switch and leave the nodes reachable from
 * nothing. The objects are NULL-prototype so that reading a field of one cannot reach a getter the page put on
 * Object.prototype. */
#include <math.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/cow.h"
#include "core/streams/stream_work.h"
#include "core/streams/readable_stream.h"
#include "core/streams/readable_stream_impl.h"
#include "core/streams/readable_byte_stream.h"

/* §4.7's controller. The four flags are the spec's own [[started]], [[pulling]], [[pullAgain]] and
   [[closeRequested]], and they mean exactly what §4.6's do. */
typedef struct {
    JSValue stream;
    JSValue pull_fn;          /* the page's `pull`, or JS_UNDEFINED */
    JSValue cancel_fn;        /* the page's `cancel`, or JS_UNDEFINED */
    JSValue source;           /* the receiver §4.9.5 invokes them on */
    JSValue queue;            /* an Array of readable byte stream queue entries */
    JSValue pending;          /* an Array of pull-into descriptors */
    JSValue byob_request;     /* §4.8's object, or JS_UNDEFINED for the spec's null */
    double  hwm;              /* [[strategyHWM]] */
    double  queue_total;      /* [[queueTotalSize]], IN BYTES */
    uint64_t auto_alloc;      /* [[autoAllocateChunkSize]], 0 meaning undefined */
    uint32_t qhead;           /* how many queue entries have left */
    uint32_t phead;           /* how many pull-into descriptors have been shifted */
    uint8_t started;
    uint8_t pulling;
    uint8_t pull_again;
    uint8_t close_requested;
} ByteCtrlData;

/* §4.8's BYOB request: the parent controller (undefined once invalidated) and the view to write into (null
   once invalidated). */
typedef struct {
    JSValue controller;
    JSValue view;
} ByobReqData;

static JSClassID g_bctrl_class, g_byobreq_class;
static JSRuntime *g_bs_rt;
static int g_bctrl_stepids[3] = { -1, -1, -1 };
static int g_byobreq_stepids[2] = { -1, -1 };
static int g_byte_rxn_stepids[4] = { -1, -1, -1, -1 };
static int g_byob_read_stepid = -1;

/* ---- the records ------------------------------------------------------------------------------------------ */

static void bctrl_finalizer(JSRuntime *rt, JSValue val)
{
    ByteCtrlData *c = JS_GetOpaque(val, g_bctrl_class);
    if (!c) return;
    JS_FreeValueRT(rt, c->stream);
    JS_FreeValueRT(rt, c->pull_fn);
    JS_FreeValueRT(rt, c->cancel_fn);
    JS_FreeValueRT(rt, c->source);
    JS_FreeValueRT(rt, c->queue);
    JS_FreeValueRT(rt, c->pending);
    JS_FreeValueRT(rt, c->byob_request);
    js_free_rt(rt, c);
}

static void bctrl_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ByteCtrlData *c = JS_GetOpaque(val, g_bctrl_class);
    if (!c) return;
    JS_MarkValue(rt, c->stream, mark_func);
    JS_MarkValue(rt, c->pull_fn, mark_func);
    JS_MarkValue(rt, c->cancel_fn, mark_func);
    JS_MarkValue(rt, c->source, mark_func);
    JS_MarkValue(rt, c->queue, mark_func);
    JS_MarkValue(rt, c->pending, mark_func);
    JS_MarkValue(rt, c->byob_request, mark_func);
}

static void byobreq_finalizer(JSRuntime *rt, JSValue val)
{
    ByobReqData *q = JS_GetOpaque(val, g_byobreq_class);
    if (!q) return;
    JS_FreeValueRT(rt, q->controller);
    JS_FreeValueRT(rt, q->view);
    js_free_rt(rt, q);
}

static void byobreq_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ByobReqData *q = JS_GetOpaque(val, g_byobreq_class);
    if (!q) return;
    JS_MarkValue(rt, q->controller, mark_func);
    JS_MarkValue(rt, q->view, mark_func);
}

/* WHAT EACH RECORD OWNS, AND WHERE IT IS CAPTURED — the same discipline readable_stream.c states at length:
   the capture is in the ACCESSOR, because a record a flow has REACHED is one it may write, and the offset list
   is the SAME list the finalizer frees. */
#define BS_OFF(T, f) (uint16_t)offsetof(T, f)
#define BS_NVAL(a)   (int)(sizeof(a) / sizeof((a)[0]))
static const uint16_t BCTRL_VALS[] = {
    BS_OFF(ByteCtrlData, stream), BS_OFF(ByteCtrlData, pull_fn), BS_OFF(ByteCtrlData, cancel_fn),
    BS_OFF(ByteCtrlData, source), BS_OFF(ByteCtrlData, queue), BS_OFF(ByteCtrlData, pending),
    BS_OFF(ByteCtrlData, byob_request),
};
static const CowRecord BCTRL_REC = { sizeof(ByteCtrlData), BCTRL_VALS, BS_NVAL(BCTRL_VALS) };

static const uint16_t BYOBREQ_VALS[] = {
    BS_OFF(ByobReqData, controller), BS_OFF(ByobReqData, view),
};
static const CowRecord BYOBREQ_REC = { sizeof(ByobReqData), BYOBREQ_VALS, BS_NVAL(BYOBREQ_VALS) };

static ByteCtrlData *bctrl_of(JSValueConst v)
{
    ByteCtrlData *c = g_bctrl_class ? JS_GetOpaque(v, g_bctrl_class) : NULL;
    if (c) cow_capture_host_record(v, c, &BCTRL_REC);
    return c;
}

static ByobReqData *byobreq_of(JSValueConst v)
{
    ByobReqData *q = g_byobreq_class ? JS_GetOpaque(v, g_byobreq_class) : NULL;
    if (q) cow_capture_host_record(v, q, &BYOBREQ_REC);
    return q;
}

/* WRITE ONE OWNED SLOT OF EITHER RECORD, and never `JS_FreeValue(ctx, c->f); c->f = <build one>;` — see cow.h
   for the order and the defect. §4.7's two ResetQueue-shaped operations are the live ones: each frees an Array
   and then allocates its replacement, and an allocation IS a collection (js_trigger_gc has exactly one caller,
   JS_NewObjectFromShape), so the slot the collector reaches through this component's gc_mark names storage
   already back on the allocator's free list.
   Each record binds its layout ONCE, here, so no site can pass a slot from one with the layout of the other.
   The MINTS do not come here: each fills its block before JS_SetOpaque, where the record is unreachable by the
   collector and its slots hold no value to release. */
static void bc_set(JSContext *ctx, ByteCtrlData *c, JSValue *slot, JSValue v)
{
    cow_record_set(ctx, c, &BCTRL_REC, slot, v);
}
static void bq_set(JSContext *ctx, ByobReqData *q, JSValue *slot, JSValue v)
{
    cow_record_set(ctx, q, &BYOBREQ_REC, slot, v);
}

bool readable_byte_ctrl_is(JSValueConst v)
{
    return g_bctrl_class != 0 && JS_GetOpaque(v, g_bctrl_class) != NULL;
}

/* ---- the two record kinds the spec describes as STRUCTS ---------------------------------------------------- */

/* A readable byte stream QUEUE ENTRY and a PULL-INTO DESCRIPTOR, as NULL-prototype objects: a field read off
   one cannot reach anything of the page's, and every write to one is a property write the COW delta captures. */
static const char *const Q_BUFFER = "buffer";
static const char *const Q_OFFSET = "byteOffset";
static const char *const Q_LENGTH = "byteLength";
static const char *const D_BUFLEN = "bufferByteLength";
static const char *const D_FILLED = "bytesFilled";
static const char *const D_MINFILL = "minimumFill";
static const char *const D_ELEM   = "elementSize";
static const char *const D_VIEW   = "viewType";
static const char *const D_READER = "readerType";

/* §4.7's "reader type": which kind of reader asked for this pull-into, or that the reader was RELEASED while
   it was outstanding — in which case whatever arrives for it goes to the queue instead. */
enum { RT_DEFAULT = 0, RT_BYOB, RT_NONE };

static double rec_num(JSContext *ctx, JSValueConst rec, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, rec, name);
    double x = 0;
    JS_ToFloat64(ctx, &x, v);
    JS_FreeValue(ctx, v);
    return x;
}

static void rec_set(JSContext *ctx, JSValueConst rec, const char *name, double x)
{
    JS_SetPropertyStr(ctx, rec, name, JS_NewFloat64(ctx, x));
}

static JSValue rec_obj(JSContext *ctx, JSValueConst rec, const char *name)   /* OWNED */
{
    return JS_GetPropertyStr(ctx, rec, name);
}

static uint32_t arr_len(JSContext *ctx, JSValueConst a)
{
    JSValue v = JS_GetPropertyStr(ctx, a, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

static void arr_push(JSContext *ctx, JSValueConst a, JSValue v)   /* CONSUMES v */
{
    JS_SetPropertyUint32(ctx, a, arr_len(ctx, a), v);
}

/* ---- §8.3's buffer operations ------------------------------------------------------------------------------ */

/* §8.3's TransferArrayBuffer: the buffer is DETACHED and a new one over the same bytes is answered. `buf` is
   BORROWED; the answer is owned, or JS_EXCEPTION with the TypeError §8.3's DetachArrayBuffer throws — which is
   also what a detached or shared buffer answers, because neither can be transferred. */
static JSValue byte_transfer(JSContext *ctx, JSValueConst buf)
{
    size_t len = 0;
    uint8_t *p;
    JSValue nb;

    /* §8.3's CanTransferArrayBuffer: a SHARED buffer cannot be detached, so it cannot be transferred, and a
       stream that copied it instead would leave the producer and the consumer writing the same bytes at once.
       The class test is the whole check here — JS_GetArrayBuffer takes both kinds. */
    if (!JS_IsArrayBuffer(buf))
        return JS_ThrowTypeError(ctx, "a byte stream's buffer must be a transferable ArrayBuffer");
    p = JS_GetArrayBuffer(ctx, &len, buf);
    if (!p) return JS_EXCEPTION;
    nb = JS_NewArrayBufferCopy(ctx, p, len);
    if (JS_IsException(nb)) return nb;
    JS_DetachArrayBuffer(ctx, buf);
    return nb;
}

/* THE VIEW-CONSTRUCTION REQUEST BUFFER, AS A TYPE. step_construct_run's operand shape is [ctor, args…] and
   « buffer, byteOffset, length » is three arguments, so the buffer is 1 + 3 slots wide. It is a TYPE for the
   reason EventFireCb is one: a width every call site restates is a width every call site is free to be behind
   on, and the slot past the end of a `JSValue cb[4]` is whatever struct field happens to follow it. */
#define BYTE_VIEW_CB_SLOTS (1 + 3)
typedef JSValue ByteViewCb[BYTE_VIEW_CB_SLOTS];

/* ! Construct(ctor, « buffer, byteOffset, length ») for the constructors §4.9.5 names — AS A REQUEST, because
   one of those constructors cannot be reached any other way. The typed array constructors are plain C bodies
   and JS_NewTypedArray IS the engine's one implementation of them, so `view_type >= 0` answers on the spot.
   %DataView% is a STEP MACHINE (JS_CFUNC_step_ctor / STEPDEF_DATAVIEW_CTOR), so there is no C entry to it that
   is not a second implementation of a constructor — and js_call_c_function DFAILs on a step callee precisely
   so that nobody writes one. `view_type < 0` therefore CONSTRUCTS it, from a controller that is already a
   machine and can hold the suspension.
   The constructor is the REALM's intrinsic, read through JS_GetClassCtor rather than off
   `DataView.prototype.constructor` — that property is the page's to overwrite, and the standard's `!` names
   the intrinsic. Per realm, because a C member runs in the realm that defined it.
   `phase` and `cb` belong to the CALLING machine; `cb` is a ByteViewCb passed through STEP_CB so its capacity
   travels with it. Returns JS_STEP_CONSTRUCT (return it) or 0 with *pout set — which may be JS_EXCEPTION, the
   encoding both a failed JS_NewTypedArray and an abrupt construct request use. */
static int byte_view_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap, int view_type,
                         JSValueConst buffer, double offset, double len, JSValue in,
                         JSValue *pout, JSValue **out_cb, int *out_argc)
{
    JSValueConst argv[3];
    JSValue o, l, ctor;
    int r;

    /* ASKED ON BOTH LEGS, because the resume leg forwards the same capacity and a caller that got the first
       one right by accident must not get the second one wrong in silence. */
    DCHECK(cb_cap >= BYTE_VIEW_CB_SLOTS,
           "a view construction was handed a buffer narrower than « buffer, byteOffset, length » — declare it "
           "ByteViewCb rather than counting the slots");

    /* THE RESUME LEG READS NONE OF THE OPERANDS: `cb` is what carried them across the suspension, so a caller
       resuming this hands over only which arm it is. */
    if (*phase != 0) {
        DCHECK(view_type < 0,
               "a typed-array view construction resumed — only the %DataView% arm has a request to park on");
        r = step_construct_run(ctx, phase, cb, cb_cap, JS_UNDEFINED, 3, NULL, in, pout, out_cb, out_argc);
        DCHECK(r == 0, "a %DataView% construct request resumed into something other than its answer");
        return r;
    }
    if (view_type >= 0) {
        JS_FreeValue(ctx, in);
        o = JS_NewFloat64(ctx, offset);
        l = JS_NewFloat64(ctx, len);
        argv[0] = buffer;
        argv[1] = o;
        argv[2] = l;
        *pout = JS_NewTypedArray(ctx, 3, argv, (JSTypedArrayEnum)view_type);
        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, l);
        return 0;
    }
    ctor = JS_GetClassCtor(ctx, JS_CLASS_DATAVIEW);
    o = JS_NewFloat64(ctx, offset);
    l = JS_NewFloat64(ctx, len);
    argv[0] = buffer;
    argv[1] = o;
    argv[2] = l;
    /* step_construct_run DUPS every operand into the request buffer, which is what holds them across the
       suspension — so this realm's %DataView% and the two numbers are released here and the parked construct
       still owns one reference to each. */
    r = step_construct_run(ctx, phase, cb, cb_cap, ctor, 3, argv, in, pout, out_cb, out_argc);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, o);
    JS_FreeValue(ctx, l);
    DCHECK(r == JS_STEP_CONSTRUCT, "the %DataView% construct request answered without parking");
    return r;
}

/* THE SAME CONSTRUCTION AT A SITE THAT CANNOT PARK. §4.9.5 names %Uint8Array% outright in four places — the
   BYOB request's view, FillReadRequestFromQueue's chunk, an enqueue answering a default reader, and the
   auto-allocated descriptor — and that arm answers on the spot. This is not a second implementation: it is the
   one above with the answer taken immediately, and the DCHECK is what keeps that true, so a site that starts
   passing a descriptor's own view type through here crashes rather than reaching a request nobody can answer. */
static JSValue byte_uint8_view(JSContext *ctx, JSValueConst buffer, double offset, double len)
{
    uint8_t phase = 0;
    ByteViewCb cb;
    JSValue out = JS_UNDEFINED, *ocb = NULL;
    int oargc = 0, i, r;

    STEP_CB_FOREACH(cb, i) cb[i] = JS_UNDEFINED;
    r = byte_view_run(ctx, &phase, STEP_CB(cb), (int)JS_TYPED_ARRAY_UINT8, buffer, offset, len,
                      JS_UNDEFINED, &out, &ocb, &oargc);
    DCHECK(r == 0, "a %Uint8Array% view construction parked — only the %DataView% arm has a request to park on");
    return out;
}

/* WHAT A VIEW IS OVER — [[ViewedArrayBuffer]], [[ByteOffset]], [[ByteLength]], and the element size and
   constructor the typed array constructors table gives for its [[TypedArrayName]]. One operation, because every
   §4.9.5 site that takes a view needs all of it and a DETACHED buffer is the same TypeError at each of them.
   `*pview_type` is the JSTypedArrayEnum, or -1 for a DataView (element size 1). The BUFFER is owned. */
static JSValue byte_view_parts(JSContext *ctx, JSValueConst view, double *poff, double *plen,
                               double *pelem, int *pview_type)
{
    size_t off = 0, len = 0, bpe = 1;
    JSValue buf;
    int vt = JS_GetTypedArrayType(view);

    if (vt >= 0) {
        buf = JS_GetTypedArrayBuffer(ctx, view, &off, &len, &bpe);
    } else if (JS_IsDataView(view)) {
        buf = JS_GetArrayBufferView(ctx, view, &off, &len);
        bpe = 1;
    } else {
        return JS_ThrowTypeError(ctx, "not an ArrayBufferView");
    }
    if (JS_IsException(buf)) return buf;
    *poff = (double)off;
    *plen = (double)len;
    *pelem = (double)bpe;
    *pview_type = vt;
    return buf;
}

/* ---- §4.9.5's queue and pull-into operations, none of which run the page's code ---------------------------- */

/* §4.9.5's ReadableByteStreamControllerInvalidateBYOBRequest. */
static void byte_invalidate_byob(JSContext *ctx, ByteCtrlData *c)
{
    ByobReqData *q;

    if (JS_IsUndefined(c->byob_request)) return;
    q = byobreq_of(c->byob_request);
    DCHECK(q != NULL, "a byte controller held something that is not a ReadableStreamBYOBRequest");
    if (q) {
        bq_set(ctx, q, &q->controller, JS_UNDEFINED);
        bq_set(ctx, q, &q->view, JS_NULL);
    }
    bc_set(ctx, c, &c->byob_request, JS_UNDEFINED);
}

/* §4.9.5's ReadableByteStreamControllerClearPendingPullIntos. */
static void byte_clear_pending(JSContext *ctx, ByteCtrlData *c)
{
    JSValue list;

    byte_invalidate_byob(ctx, c);
    /* BUILT INTO A LOCAL AND THEN PUBLISHED: the new Array is minted while the OLD one is still live and still
       named by the record, so the collection this allocation can start does not reach a freed slot. */
    list = JS_NewArray(ctx);
    CHECK(!JS_IsException(list), "a byte stream's pull-into list could not be allocated");
    bc_set(ctx, c, &c->pending, list);
    c->phead = 0;
}

/* §8.1's ResetQueue over §4.7's queue: the entries and the byte total go together. */
static void byte_reset_queue(JSContext *ctx, ByteCtrlData *c)
{
    JSValue q = JS_NewArray(ctx);

    /* BUILT INTO A LOCAL AND THEN PUBLISHED, for the reason byte_clear_pending states one screen up. */
    CHECK(!JS_IsException(q), "a byte stream's queue could not be allocated");
    bc_set(ctx, c, &c->queue, q);
    c->qhead = 0;
    c->queue_total = 0;
}

/* §4.9.5's ReadableByteStreamControllerShiftPendingPullInto. The descriptor is OWNED by the caller. */
static JSValue byte_shift_pending(JSContext *ctx, ByteCtrlData *c)
{
    JSValue d;

    DCHECK(JS_IsUndefined(c->byob_request), "a pull-into was shifted while a BYOB request still named it");
    DCHECK(c->phead < arr_len(ctx, c->pending), "a pull-into was shifted off an empty list");
    d = JS_GetPropertyUint32(ctx, c->pending, c->phead);
    JS_SetPropertyUint32(ctx, c->pending, c->phead, JS_UNDEFINED);
    c->phead++;
    return d;
}

static uint32_t byte_pending_n(JSContext *ctx, ByteCtrlData *c)
{
    return arr_len(ctx, c->pending) - c->phead;
}

static uint32_t byte_queue_n(JSContext *ctx, ByteCtrlData *c)
{
    return arr_len(ctx, c->queue) - c->qhead;
}

/* The first pull-into descriptor, BORROWED-as-owned (the caller frees). */
static JSValue byte_first_pending(JSContext *ctx, ByteCtrlData *c)
{
    DCHECK(byte_pending_n(ctx, c) > 0, "the first pull-into of an empty list was asked for");
    return JS_GetPropertyUint32(ctx, c->pending, c->phead);
}

/* §4.9.5's ReadableByteStreamControllerEnqueueChunkToQueue. `buffer` is CONSUMED. */
static int byte_enqueue_chunk(JSContext *ctx, ByteCtrlData *c, JSValue buffer, double off, double len)
{
    JSValue e = JS_NewObjectProto(ctx, JS_NULL);

    if (JS_IsException(e)) { JS_FreeValue(ctx, buffer); return -1; }
    JS_DefinePropertyValueStr(ctx, e, Q_BUFFER, buffer, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, Q_OFFSET, JS_NewFloat64(ctx, off), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, Q_LENGTH, JS_NewFloat64(ctx, len), JS_PROP_C_W_E);
    arr_push(ctx, c->queue, e);
    c->queue_total += len;
    return 0;
}

/* §4.9.5's ReadableByteStreamControllerEnqueueClonedChunkToQueue, whose CloneArrayBuffer is a copy of the
   range: the source buffer is left alone, because the descriptor it belongs to is about to be reused. Returns
   -1 with the exception live, which is what its caller ERRORS the controller with. */
static int byte_enqueue_cloned(JSContext *ctx, ByteCtrlData *c, JSValueConst buffer, double off, double len)
{
    size_t blen = 0;
    uint8_t *p = JS_GetArrayBuffer(ctx, &blen, buffer);
    JSValue nb;

    if (!p) return -1;
    CHECK(off >= 0 && len >= 0 && off + len <= (double)blen,
          "a byte stream cloned a range that is not inside its buffer");
    nb = JS_NewArrayBufferCopy(ctx, p + (size_t)off, (size_t)len);
    if (JS_IsException(nb)) return -1;
    return byte_enqueue_chunk(ctx, c, nb, 0, len);
}

/* §4.9.5's ReadableByteStreamControllerEnqueueDetachedPullIntoToQueue: a pull-into whose reader is gone keeps
   whatever was already written into it, as queue bytes. */
static int byte_enqueue_detached(JSContext *ctx, ByteCtrlData *c, JSValueConst dsc)
{
    double filled = rec_num(ctx, dsc, D_FILLED);
    int r = 0;

    DCHECK((int)rec_num(ctx, dsc, D_READER) == RT_NONE,
           "a pull-into with a live reader was enqueued to the queue as detached");
    if (filled > 0) {
        JSValue buf = rec_obj(ctx, dsc, Q_BUFFER);
        r = byte_enqueue_cloned(ctx, c, buf, rec_num(ctx, dsc, Q_OFFSET), filled);
        JS_FreeValue(ctx, buf);
    }
    if (r == 0) JS_FreeValue(ctx, byte_shift_pending(ctx, c));
    return r;
}

/* §4.9.5's ReadableByteStreamControllerFillPullIntoDescriptorFromQueue, with its FillHeadPullIntoDescriptor
   inlined at the one place the standard calls it from. `*pready` is the standard's `ready`. */
static int byte_fill_from_queue(JSContext *ctx, ByteCtrlData *c, JSValueConst dsc, bool *pready)
{
    double filled = rec_num(ctx, dsc, D_FILLED);
    double blen   = rec_num(ctx, dsc, Q_LENGTH);
    double elem   = rec_num(ctx, dsc, D_ELEM);
    double minf   = rec_num(ctx, dsc, D_MINFILL);
    double doff   = rec_num(ctx, dsc, Q_OFFSET);
    double room   = blen - filled;
    double maxcopy = c->queue_total < room ? c->queue_total : room;
    double maxfilled = filled + maxcopy;
    double remain = maxcopy;
    double maxaligned = maxfilled - fmod(maxfilled, elem);
    JSValue dbuf;
    uint8_t *dp;
    size_t dcap = 0;

    DCHECK(JS_IsUndefined(c->byob_request), "a pull-into was filled while a BYOB request still named it");
    DCHECK(filled < minf, "a pull-into already past its minimum fill was filled again");
    *pready = false;
    if (maxaligned >= minf) {
        /* A descriptor that is not yet filled up to its minimum length STAYS at the head of the list, so the
           underlying source can keep filling it. */
        remain = maxaligned - filled;
        *pready = true;
    }
    dbuf = rec_obj(ctx, dsc, Q_BUFFER);
    dp = JS_GetArrayBuffer(ctx, &dcap, dbuf);
    if (!dp) { JS_FreeValue(ctx, dbuf); return -1; }
    while (remain > 0) {
        JSValue head = JS_GetPropertyUint32(ctx, c->queue, c->qhead);
        double hoff = rec_num(ctx, head, Q_OFFSET);
        double hlen = rec_num(ctx, head, Q_LENGTH);
        double n = remain < hlen ? remain : hlen;
        double dest = doff + filled;
        JSValue hbuf = rec_obj(ctx, head, Q_BUFFER);
        size_t scap = 0;
        uint8_t *sp = JS_GetArrayBuffer(ctx, &scap, hbuf);

        /* §4.9.5's CanCopyDataBlockBytes, which the standard says the user agent should ALWAYS check and stop
           on: the next line reads and writes raw memory, so a violated bound is not a spec bug to report but a
           process to end. */
        CHECK(sp != NULL && sp != dp, "a byte stream copied a queue entry into the buffer it came from");
        CHECK(dest >= 0 && n >= 0 && dest + n <= (double)dcap && hoff >= 0 && hoff + n <= (double)scap,
              "a byte stream's queue copy left the bounds of one of its buffers");
        memcpy(dp + (size_t)dest, sp + (size_t)hoff, (size_t)n);
        JS_FreeValue(ctx, hbuf);
        if (hlen == n) {
            JS_SetPropertyUint32(ctx, c->queue, c->qhead, JS_UNDEFINED);
            c->qhead++;
        } else {
            rec_set(ctx, head, Q_OFFSET, hoff + n);
            rec_set(ctx, head, Q_LENGTH, hlen - n);
        }
        JS_FreeValue(ctx, head);
        c->queue_total -= n;
        filled += n;
        rec_set(ctx, dsc, D_FILLED, filled);
        remain -= n;
    }
    JS_FreeValue(ctx, dbuf);
    if (!*pready) {
        DCHECK(c->queue_total == 0, "a pull-into stopped short of its minimum fill with bytes still queued");
        DCHECK(filled < minf, "a pull-into reported not-ready past its minimum fill");
    }
    return 0;
}

/* §4.9.5's ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue. The answer is an Array of the
   descriptors that filled, in order — the list the caller then COMMITS. */
static JSValue byte_process_pending(JSContext *ctx, ByteCtrlData *c)
{
    JSValue filled = JS_NewArray(ctx);

    if (JS_IsException(filled)) return filled;
    DCHECK(!c->close_requested, "pending pull-intos were filled from the queue of a closing byte stream");
    while (byte_pending_n(ctx, c) > 0) {
        JSValue dsc;
        bool ready = false;

        if (c->queue_total == 0) break;
        dsc = byte_first_pending(ctx, c);
        if (byte_fill_from_queue(ctx, c, dsc, &ready) < 0) {
            JS_FreeValue(ctx, dsc);
            JS_FreeValue(ctx, filled);
            return JS_EXCEPTION;
        }
        if (!ready) { JS_FreeValue(ctx, dsc); break; }
        JS_FreeValue(ctx, byte_shift_pending(ctx, c));
        arr_push(ctx, filled, dsc);
    }
    return filled;
}

/* §4.9.5's ReadableByteStreamControllerConvertPullIntoDescriptor, AS A REQUEST: the descriptor's buffer is
   TRANSFERRED and the answer is a view of the descriptor's OWN type over the bytes written into it — and a
   descriptor made for a DataView names %DataView%, so step 6's Construct is the request byte_view_run makes.
   The phase byte is SHARED with that construct and is why this is one function rather than a transfer followed
   by a view: a resume must not detach the buffer a second time. It is the CALLER's byte, and it must be the
   caller's OWN — a machine that handed this the phase it also parks its result-delivery call on would come
   back at phase 1 and skip its own build block. */
static int byte_convert_pull_into_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap,
                                      JSValueConst dsc, JSValue in, JSValue *pout,
                                      JSValue **out_cb, int *out_argc)
{
    int vt = (int)rec_num(ctx, dsc, D_VIEW);
    double filled, elem;
    JSValue buf, nb;
    int r;

    if (*phase != 0)
        return byte_view_run(ctx, phase, cb, cb_cap, vt, JS_UNDEFINED, 0, 0, in, pout, out_cb, out_argc);

    filled = rec_num(ctx, dsc, D_FILLED);
    elem   = rec_num(ctx, dsc, D_ELEM);
    buf    = rec_obj(ctx, dsc, Q_BUFFER);
    DCHECK(filled <= rec_num(ctx, dsc, Q_LENGTH), "a pull-into was filled past the room it declared");
    DCHECK(fmod(filled, elem) == 0, "a pull-into was converted at a fill that is not a whole number of elements");
    nb = byte_transfer(ctx, buf);
    JS_FreeValue(ctx, buf);
    if (JS_IsException(nb)) {
        JS_FreeValue(ctx, in);
        *pout = JS_EXCEPTION;
        return 0;
    }
    JS_SetPropertyStr(ctx, dsc, Q_BUFFER, JS_DupValue(ctx, nb));
    r = byte_view_run(ctx, phase, cb, cb_cap, vt, nb, rec_num(ctx, dsc, Q_OFFSET), filled / elem, in, pout,
                      out_cb, out_argc);
    JS_FreeValue(ctx, nb);
    return r;
}

/* §4.9.5's ReadableByteStreamControllerPullInto, closed branch: "! Construct(ctor, « pullIntoDescriptor's
   buffer, pullIntoDescriptor's byte offset, 0 »)" — the EMPTY view whose whole job is to hand the caller its
   backing memory back, and which names the SAME constructor, so a BYOB read over a DataView parks here too. */
static int byte_empty_view_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap, JSValueConst dsc,
                               JSValue in, JSValue *pout, JSValue **out_cb, int *out_argc)
{
    int vt = (int)rec_num(ctx, dsc, D_VIEW);
    JSValue buf;
    int r;

    if (*phase != 0)
        return byte_view_run(ctx, phase, cb, cb_cap, vt, JS_UNDEFINED, 0, 0, in, pout, out_cb, out_argc);
    buf = rec_obj(ctx, dsc, Q_BUFFER);
    r = byte_view_run(ctx, phase, cb, cb_cap, vt, buf, rec_num(ctx, dsc, Q_OFFSET), 0, in, pout,
                      out_cb, out_argc);
    JS_FreeValue(ctx, buf);
    return r;
}

/* §4.9.5's ReadableByteStreamControllerGetBYOBRequest. */
static JSValue byte_get_byob_request(JSContext *ctx, ByteCtrlData *c, JSValueConst ctrl)
{
    JSValue dsc, buf, view, obj;
    ByobReqData *q;

    if (!JS_IsUndefined(c->byob_request)) return JS_DupValue(ctx, c->byob_request);
    if (byte_pending_n(ctx, c) == 0) return JS_NULL;
    dsc = byte_first_pending(ctx, c);
    buf = rec_obj(ctx, dsc, Q_BUFFER);
    view = byte_uint8_view(ctx, buf,
                           rec_num(ctx, dsc, Q_OFFSET) + rec_num(ctx, dsc, D_FILLED),
                           rec_num(ctx, dsc, Q_LENGTH) - rec_num(ctx, dsc, D_FILLED));
    JS_FreeValue(ctx, buf);
    JS_FreeValue(ctx, dsc);
    if (JS_IsException(view)) return view;
    {
        JSValue proto = JS_GetClassProto(ctx, g_byobreq_class);
        DCHECK(!JS_IsNull(proto), "a BYOB request was minted in a realm that never ran its install");
        obj = JS_NewObjectProtoClass(ctx, proto, g_byobreq_class);
        JS_FreeValue(ctx, proto);
    }
    if (JS_IsException(obj)) { JS_FreeValue(ctx, view); return obj; }
    q = js_mallocz(ctx, sizeof *q);
    if (!q) { JS_FreeValue(ctx, obj); JS_FreeValue(ctx, view); return JS_EXCEPTION; }
    q->controller = JS_DupValue(ctx, ctrl);
    q->view = view;
    JS_SetOpaque(obj, q);
    c->byob_request = JS_DupValue(ctx, obj);
    return obj;
}

/* §4.9.5's ReadableByteStreamControllerGetDesiredSize — the number-or-null, over the BYTE total. */
static JSValue byte_desired_size(JSContext *ctx, ByteCtrlData *c)
{
    StreamData *d = rs_stream_data(c->stream);

    DCHECK(d != NULL, "a byte controller's stream stopped being a ReadableStream");
    if (d->state == RS_ERRORED) return JS_NULL;
    if (d->state == RS_CLOSED) return JS_NewInt32(ctx, 0);
    return JS_NewFloat64(ctx, c->hwm - c->queue_total);
}

/* §4.9.5's ReadableByteStreamControllerShouldCallPull. */
static bool byte_should_pull(JSContext *ctx, ByteCtrlData *c)
{
    StreamData *d = rs_stream_data(c->stream);

    DCHECK(d != NULL, "a byte controller's stream stopped being a ReadableStream");
    if (d->state != RS_READABLE) return false;
    if (c->close_requested) return false;
    if (!c->started) return false;
    /* ONE question for both reader kinds: a parked read request and a parked read-into request are the same
       two arrays, because a stream has one reader and a reader has one of the two lists. */
    if (!JS_IsUndefined(d->reader) && rs_read_pending(ctx, d) > 0) return true;
    return c->hwm - c->queue_total > 0;
}

/* ---- §4.9.5's reactions ------------------------------------------------------------------------------------ */

enum { BRX_START_OK = 0, BRX_START_ERR, BRX_PULL_OK, BRX_PULL_ERR };

#define BRX_STAGES(X) \
    X(BRX_DECIDE, "Streams §4.9.5 SetUpReadableByteStreamController steps 15-16 / " \
                  "ReadableByteStreamControllerCallPullIfNeeded steps 7-8 (which reaction this is: the " \
                  "controller is started, a pull may repeat, or an error is to be reported)") \
    X(BRX_RUN, "Streams §4.9.5 ReadableByteStreamControllerCallPullIfNeeded steps 2-8 / §4.9.1 " \
               "ReadableStreamError (the pull the reaction asks for, or the error it reports)")
enum { BRX_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const BRX_STEPS[] = { BRX_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int byte_react(JSContext *ctx, JSValueConst promise, JSValueConst ctrl, int ok, int err)
{
    JSValueConst data[1];

    data[0] = ctrl;
    return stream_react(ctx, promise, g_byte_rxn_stepids[ok], g_byte_rxn_stepids[err], data, 1);
}

/* §4.9.5's ReadableByteStreamControllerCallPullIfNeeded — the same three page-code points §4.6's has, over a
   different ShouldCallPull. The caller sets `w->pull = P_TEST` and calls until it returns 0. */
int readable_byte_pull_run(JSContext *ctx, StreamWork *w, JSValueConst ctrl_v, JSValue in,
                           JSValue **out_cb, int *out_argc)
{
    ByteCtrlData *c = bctrl_of(ctrl_v);
    int r;

    DCHECK(c != NULL, "the byte pull sequence ran on a value that is not a ReadableByteStreamController");
    if (w->pull == P_TEST) {
        if (JS_IsUndefined(c->pull_fn) || !byte_should_pull(ctx, c)) {
            JS_FreeValue(ctx, in);
            w->pull = P_IDLE;
            return 0;
        }
        if (c->pulling) {
            c->pull_again = 1;
            JS_FreeValue(ctx, in);
            w->pull = P_IDLE;
            return 0;
        }
        DCHECK(!c->pull_again, "a byte controller carried pullAgain while it was not pulling at all");
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
        /* §4.9.5 builds the pull algorithm with PromiseCall: a `pull` that THROWS yields a REJECTED PROMISE,
           and the controller is errored by the REACTION to it rather than here, inline. */
        w->pull = JS_IsException(res) ? P_REJECT : P_RESOLVE;
        w->value = JS_IsException(res) ? JS_GetException(ctx) : res;
        in = JS_UNDEFINED;
    }
    if (w->pull == P_RESOLVE || w->pull == P_REJECT) {
        r = stream_promise_of_run(ctx, w, w->pull == P_REJECT, in, out_cb, out_argc);
        if (r != 0) return r;
        w->pull = P_THEN;
    }
    DCHECK(w->pull == P_THEN, "the byte pull sequence resumed in a state it never parks in");
    r = byte_react(ctx, w->func, ctrl_v, BRX_PULL_OK, BRX_PULL_ERR);
    JS_FreeValue(ctx, w->func);
    w->func = JS_UNDEFINED;
    w->pull = P_IDLE;
    return r;
}

typedef struct {
    JSStepHdr hdr;
    StreamWork w;
} JSByteRxnState;

static void js_byte_rxn_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSByteRxnState *s = st;
    stream_work_visit(ctx, &s->w, v);
}

static int js_byte_rxn_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSByteRxnState *s = st;
    JSValueConst ctrl = JS_StepClosureData(&s->hdr, 0);
    ByteCtrlData *c = bctrl_of(ctrl);
    StreamData *d;
    int r;

    DCHECK(c != NULL, "a byte reaction captured something that is not a ReadableByteStreamController");
    d = rs_stream_data(c->stream);
    DCHECK(d != NULL, "a byte controller's stream stopped being a ReadableStream");

    if (s->hdr.stage == BRX_DECIDE) {
        int op = s->hdr.arg;
        stream_work_start(&s->w);
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->hdr.stage = BRX_RUN;
        if (op == BRX_START_OK) {
            c->started = 1;
            DCHECK(!c->pulling && !c->pull_again, "a byte controller pulled before its start promise fulfilled");
            s->w.pull = P_TEST;
        } else if (op == BRX_PULL_OK) {
            c->pulling = 0;
            if (!c->pull_again) return JS_STEP_DONE;
            c->pull_again = 0;
            s->w.pull = P_TEST;
        } else {
            DCHECK(op == BRX_START_ERR || op == BRX_PULL_ERR, "a byte reaction ran with no operation");
            if (op == BRX_PULL_ERR) c->pulling = 0;
            /* §4.9.5's ReadableByteStreamControllerError: the pull-intos go, the queue goes, the algorithms go,
               and only then does the stream error. */
            if (d->state == RS_READABLE) {
                byte_clear_pending(ctx, c);
                byte_reset_queue(ctx, c);
                readable_byte_clear_algorithms(ctx, ctrl);
            }
            s->w.err = JS_DupValue(ctx, s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED);
            s->w.settle = S_ERR_SET;
        }
    }
    r = s->w.pull != P_IDLE ? readable_byte_pull_run(ctx, &s->w, ctrl, cb_result, out_cb, out_argc)
                            : rs_settle_run(ctx, &s->w, d, cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    return JS_STEP_DONE;
}

#define BRX_DEF(i, alg) { sizeof(JSByteRxnState), js_byte_rxn_step, NULL, (i), \
                          .catches_abrupt = 1, .visit = js_byte_rxn_visit, \
                          .algorithm = (alg), .steps = BRX_STEPS }
static const JSTrampStepDef js_byte_rxn_defs[4] = {
    BRX_DEF(BRX_START_OK,
            "Streams §4.9.5 SetUpReadableByteStreamController step 15's onFulfilled"),
    BRX_DEF(BRX_START_ERR,
            "Streams §4.9.5 SetUpReadableByteStreamController step 16's onRejected"),
    BRX_DEF(BRX_PULL_OK,
            "Streams §4.9.5 ReadableByteStreamControllerCallPullIfNeeded step 7's onFulfilled"),
    BRX_DEF(BRX_PULL_ERR,
            "Streams §4.9.5 ReadableByteStreamControllerCallPullIfNeeded step 8's onRejected"),
};
#undef BRX_DEF

/* ---- THE COMMIT LOOP -------------------------------------------------------------------------------------- */

/* §4.9.5's ReadableByteStreamControllerCommitPullIntoDescriptor, for each descriptor a step produced. It is a
   LOOP OF CALLS because answering one request is the page's code (27.5.1.3 step 2.f reads `then` off the
   result object), so each iteration is a suspension point of its own. */
typedef struct {
    JSValue filled;   /* an Array of pull-into descriptors */
    JSValue func;     /* the resolving function this commit is calling */
    JSValue value;    /* what it is being called with */
    /* THE DESCRIPTOR THIS ITERATION IS CONVERTING, and the state that conversion parks on. Step 5's Convert
       constructs the descriptor's own view type, and a BYOB read over a DataView makes that a request — so the
       descriptor has to outlive the suspension, and the phase byte has to be this loop's OWN. Sharing
       StreamWork's would come back at phase 1 and skip the block that takes the next descriptor. */
    JSValue dsc;
    ByteViewCb view_cb;
    uint32_t fi;      /* how many have been committed */
    uint8_t view_phase;
    /* Step 3-4's `done`, DECIDED BEFORE THE CONVERT, which is where the standard decides it. It was read off
       the stream after the convert while the convert was a C call and could not be observed; the moment that
       became a suspension point, a read after it would be reporting the state the stream is in when the
       %DataView% construct finishes rather than the one the commit began in. */
    uint8_t done;
} ByteCommit;

static void byte_commit_start(ByteCommit *cm)
{
    int i;
    cm->filled = cm->func = cm->value = cm->dsc = JS_UNDEFINED;
    STEP_CB_FOREACH(cm->view_cb, i) cm->view_cb[i] = JS_UNDEFINED;
    cm->fi = 0;
    cm->view_phase = 0;
    cm->done = 0;
}

static void byte_commit_visit(JSContext *ctx, ByteCommit *cm, JSStepVisit *v)
{
    int i;
    v->val(ctx, &cm->filled);
    v->val(ctx, &cm->func);
    v->val(ctx, &cm->value);
    v->val(ctx, &cm->dsc);
    STEP_CB_FOREACH(cm->view_cb, i) v->val(ctx, &cm->view_cb[i]);
}

static int byte_commit_run(JSContext *ctx, StreamWork *w, ByteCommit *cm, StreamData *d,
                           JSValue in, JSValue **out_cb, int *out_argc)
{
    for (;;) {
        JSValueConst arg;
        JSValue out;
        int r;

        /* w->phase != 0 means the ANSWER is parked mid-call and the descriptor is long done with; otherwise
           this iteration is either starting (view_phase 0, no descriptor held) or resuming its convert. */
        if (w->phase == 0) {
            JSValue view = JS_UNDEFINED;

            if (cm->view_phase == 0) {
                DCHECK(JS_IsUndefined(cm->dsc), "a commit took a second pull-into while it still held one");
                if (JS_IsUndefined(cm->filled) || cm->fi >= arr_len(ctx, cm->filled)) {
                    JS_FreeValue(ctx, in);
                    return 0;
                }
                cm->dsc = JS_GetPropertyUint32(ctx, cm->filled, cm->fi++);
                DCHECK(d->state != RS_ERRORED, "a pull-into was committed into an errored stream");   /* step 1 */
                DCHECK((int)rec_num(ctx, cm->dsc, D_READER) != RT_NONE,                               /* step 2 */
                       "a pull-into whose reader was released reached the commit");
                cm->done = d->state == RS_CLOSED;                                                 /* steps 3-4 */
                DCHECK(!cm->done ||
                           fmod(rec_num(ctx, cm->dsc, D_FILLED), rec_num(ctx, cm->dsc, D_ELEM)) == 0,
                       "a closed byte stream committed a pull-into at a fill that is not a whole number of "
                       "elements");
            }
            /* STEP 5 — and the one suspension point, because a descriptor made for a DataView names a step
               machine as its view constructor. */
            r = byte_convert_pull_into_run(ctx, &cm->view_phase, STEP_CB(cm->view_cb), cm->dsc, in, &view,
                                           out_cb, out_argc);
            if (r > 0) return r;
            in = JS_UNDEFINED;
            JS_FreeValue(ctx, cm->dsc);
            cm->dsc = JS_UNDEFINED;
            if (JS_IsException(view)) return -1;
            JS_FreeValue(ctx, cm->func);
            cm->func = rs_take_read(ctx, d, 0);                                                   /* steps 6-7 */
            DCHECK(!JS_IsUndefined(cm->func), "a filled pull-into had no parked request to answer");
            JS_FreeValue(ctx, cm->value);
            cm->value = rs_read_result(ctx, view, cm->done);
            if (JS_IsException(cm->value)) { cm->value = JS_UNDEFINED; return -1; }
        }
        arg = cm->value;
        r = step_call_run(ctx, &w->phase, STEP_CB(w->cb), cm->func, JS_UNDEFINED, 1, &arg, in, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return -1;
        JS_FreeValue(ctx, out);
        in = JS_UNDEFINED;
    }
}

/* ---- §4.9.2's polymorphic entry points -------------------------------------------------------------------- */

bool readable_byte_has_chunk(JSValueConst ctrl)
{
    ByteCtrlData *c = bctrl_of(ctrl);

    DCHECK(c != NULL, "a byte [[PullSteps]] ran on a value that is not a ReadableByteStreamController");
    return c->queue_total > 0;
}

bool readable_byte_drained(JSValueConst ctrl)
{
    ByteCtrlData *c = bctrl_of(ctrl);

    DCHECK(c != NULL, "a byte HandleQueueDrain ran on a value that is not a ReadableByteStreamController");
    return c->queue_total == 0 && c->close_requested;
}

/* §4.9.5's ReadableByteStreamControllerFillReadRequestFromQueue up to its chunk steps: the head entry LEAVES
   the queue and becomes a Uint8Array over exactly its bytes. */
JSValue readable_byte_take_chunk(JSContext *ctx, JSValueConst ctrl)
{
    ByteCtrlData *c = bctrl_of(ctrl);
    JSValue e, buf, view;
    double off, len;

    DCHECK(c != NULL, "a byte [[PullSteps]] ran on a value that is not a ReadableByteStreamController");
    DCHECK(c->queue_total > 0, "a byte read took a chunk from an empty queue");
    e = JS_GetPropertyUint32(ctx, c->queue, c->qhead);
    JS_SetPropertyUint32(ctx, c->queue, c->qhead, JS_UNDEFINED);
    c->qhead++;
    off = rec_num(ctx, e, Q_OFFSET);
    len = rec_num(ctx, e, Q_LENGTH);
    buf = rec_obj(ctx, e, Q_BUFFER);
    JS_FreeValue(ctx, e);
    c->queue_total -= len;
    if (c->queue_total < 0) c->queue_total = 0;
    view = byte_uint8_view(ctx, buf, off, len);
    JS_FreeValue(ctx, buf);
    return view;
}

JSValue readable_byte_auto_allocate(JSContext *ctx, JSValueConst ctrl)
{
    ByteCtrlData *c = bctrl_of(ctrl);
    JSValue buf, dsc;

    DCHECK(c != NULL, "a byte [[PullSteps]] ran on a value that is not a ReadableByteStreamController");
    if (c->auto_alloc == 0) return JS_UNDEFINED;
    buf = JS_NewArrayBufferCopy(ctx, NULL, (size_t)c->auto_alloc);
    if (JS_IsException(buf)) return buf;
    dsc = JS_NewObjectProto(ctx, JS_NULL);
    if (JS_IsException(dsc)) { JS_FreeValue(ctx, buf); return dsc; }
    JS_DefinePropertyValueStr(ctx, dsc, Q_BUFFER, buf, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, dsc, D_BUFLEN, JS_NewFloat64(ctx, (double)c->auto_alloc), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, dsc, Q_OFFSET, JS_NewInt32(ctx, 0), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, dsc, Q_LENGTH, JS_NewFloat64(ctx, (double)c->auto_alloc), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, dsc, D_FILLED, JS_NewInt32(ctx, 0), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, dsc, D_MINFILL, JS_NewInt32(ctx, 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, dsc, D_ELEM, JS_NewInt32(ctx, 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, dsc, D_VIEW, JS_NewInt32(ctx, (int)JS_TYPED_ARRAY_UINT8), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, dsc, D_READER, JS_NewInt32(ctx, RT_DEFAULT), JS_PROP_C_W_E);
    arr_push(ctx, c->pending, dsc);
    return JS_UNDEFINED;
}

void readable_byte_release_steps(JSContext *ctx, JSValueConst ctrl)
{
    ByteCtrlData *c = bctrl_of(ctrl);
    JSValue first, list;

    DCHECK(c != NULL, "a byte [[ReleaseSteps]] ran on a value that is not a ReadableByteStreamController");
    if (byte_pending_n(ctx, c) == 0) return;
    /* §4.7's [[ReleaseSteps]]: the pull-into the released reader was filling SURVIVES, with its reader type set
       to "none" — so whatever the source writes into it next goes to the queue rather than to a reader that no
       longer exists — and every LATER pull-into is dropped with the reader that asked for it. */
    first = byte_first_pending(ctx, c);
    rec_set(ctx, first, D_READER, RT_NONE);
    list = JS_NewArray(ctx);
    CHECK(!JS_IsException(list), "a byte stream's pull-into list could not be allocated");
    JS_SetPropertyUint32(ctx, list, 0, first);
    bc_set(ctx, c, &c->pending, list);
    c->phead = 0;
    byte_invalidate_byob(ctx, c);
}

void readable_byte_clear(JSContext *ctx, JSValueConst ctrl)
{
    ByteCtrlData *c = bctrl_of(ctrl);

    DCHECK(c != NULL, "a byte [[CancelSteps]] ran on a value that is not a ReadableByteStreamController");
    byte_clear_pending(ctx, c);
    byte_reset_queue(ctx, c);
}

JSValueConst readable_byte_cancel_fn(JSValueConst ctrl)
{
    ByteCtrlData *c = bctrl_of(ctrl);

    DCHECK(c != NULL, "a byte [[CancelSteps]] ran on a value that is not a ReadableByteStreamController");
    return c->cancel_fn;
}

JSValueConst readable_byte_source(JSValueConst ctrl)
{
    ByteCtrlData *c = bctrl_of(ctrl);

    DCHECK(c != NULL, "a byte [[CancelSteps]] ran on a value that is not a ReadableByteStreamController");
    return c->source;
}

void readable_byte_clear_algorithms(JSContext *ctx, JSValueConst ctrl)
{
    ByteCtrlData *c = bctrl_of(ctrl);

    DCHECK(c != NULL, "a byte ClearAlgorithms ran on a value that is not a ReadableByteStreamController");
    bc_set(ctx, c, &c->pull_fn, JS_UNDEFINED);
    bc_set(ctx, c, &c->cancel_fn, JS_UNDEFINED);
}

/* ---- §4.9.5's SetUpReadableByteStreamController ------------------------------------------------------------ */

int readable_byte_ctrl_setup(JSContext *ctx, JSValueConst stream, JSValueConst pull_fn, JSValueConst cancel_fn,
                             JSValueConst source, double hwm, uint64_t auto_alloc)
{
    StreamData *d = rs_stream_data(stream);
    ByteCtrlData *c;
    JSValue obj;

    DCHECK(d != NULL, "a byte controller was set up on a value that is not a ReadableStream");
    {
        JSValue proto = JS_GetClassProto(ctx, g_bctrl_class);
        DCHECK(!JS_IsNull(proto), "a §4.7 controller was minted in a realm that never ran its install");
        obj = JS_NewObjectProtoClass(ctx, proto, g_bctrl_class);
        JS_FreeValue(ctx, proto);
    }
    if (JS_IsException(obj)) return -1;
    c = js_mallocz(ctx, sizeof *c);
    if (!c) { JS_FreeValue(ctx, obj); return -1; }
    c->pull_fn = c->cancel_fn = c->source = c->byob_request = JS_UNDEFINED;
    c->stream = JS_DupValue(ctx, stream);
    c->queue = JS_NewArray(ctx);
    c->pending = JS_NewArray(ctx);
    JS_SetOpaque(obj, c);
    if (JS_IsException(c->queue) || JS_IsException(c->pending)) { JS_FreeValue(ctx, obj); return -1; }
    c->pull_fn = JS_DupValue(ctx, pull_fn);
    c->cancel_fn = JS_DupValue(ctx, cancel_fn);
    c->source = JS_DupValue(ctx, source);
    c->hwm = hwm;
    c->auto_alloc = auto_alloc;
    /* §4.2's constructor builds the stream with §4.6's controller already attached (there is no stream without
       one), so THIS is where a byte stream loses it: the two are alternatives, and a stream carrying both would
       answer §4.9.2's polymorphic call from whichever one a reader happened to find. */
    rs_stream_set(ctx, d, &d->controller, obj);
    return 0;
}

int readable_byte_start(JSContext *ctx, JSValueConst stream, JSValueConst start_promise)
{
    StreamData *d = rs_stream_data(stream);

    DCHECK(d != NULL, "a byte controller's start ran on a value that is not a ReadableStream");
    return byte_react(ctx, start_promise, d->controller, BRX_START_OK, BRX_START_ERR);
}

/* ---- §4.7's MEMBERS: enqueue, close, error ------------------------------------------------------------------ */

enum { BC_ENQUEUE = 0, BC_CLOSE, BC_ERROR };

#define BS_STAGES(X) \
    X(BS_START, "Streams §4.7 enqueue(chunk) / close() / error(e) steps 1-5 and the §4.9.5 operation each " \
                "performs, up to its first run of the page's code") \
    X(BS_DRAIN, "Streams §4.9.5 ReadableByteStreamControllerProcessReadRequestsUsingQueue (each queued entry " \
                "answers one parked read request, and the HandleQueueDrain between them may pull)") \
    X(BS_FEED, "Streams §4.9.5 ReadableByteStreamControllerEnqueue step 8.3.4 (the transferred chunk answers " \
               "the waiting reader directly rather than being queued)") \
    X(BS_COMMIT, "Streams §4.9.5 ReadableByteStreamControllerCommitPullIntoDescriptor (each filled pull-into " \
                 "answers one parked read-into request)") \
    X(BS_SETTLE, "Streams §4.9.1 ReadableStreamClose / ReadableStreamError (the reader's `closed` promise and " \
                 "every parked request)") \
    X(BS_RETHROW, "Streams §4.9.5 ReadableByteStreamControllerClose step 4.1.3 (a close at a misaligned fill " \
                  "errors the stream and then throws the same TypeError at its caller)") \
    X(BS_PULL, "Streams §4.9.5 ReadableByteStreamControllerCallPullIfNeeded")
enum { BS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const BS_STEPS[] = { BS_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* The drain loop's own cursor — a position inside one stage, the way StreamWork's `phase` is. */
enum { L_NEXT = 0, L_DRAIN, L_CALL };

typedef struct {
    JSStepHdr hdr;
    StreamWork w;
    ByteCommit cm;
    JSValue chunk_buf;      /* the TRANSFERRED buffer an enqueue is placing */
    JSValue rethrow;
    double  chunk_off, chunk_len;
    uint8_t sub;
} JSByteCtrlState;

static void js_byte_ctrl_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSByteCtrlState *s = st;
    stream_work_visit(ctx, &s->w, v);
    byte_commit_visit(ctx, &s->cm, v);
    v->val(ctx, &s->chunk_buf);
    v->val(ctx, &s->rethrow);
}

static int js_byte_ctrl_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSByteCtrlState *s = st;
    ByteCtrlData *c = bctrl_of(s->hdr.this_val);
    StreamData *d;
    int r;

    if (!c) {
        JS_FreeValue(ctx, cb_result);
        JS_ThrowTypeError(ctx, "not a ReadableByteStreamController");
        return JS_STEP_ABRUPT;
    }
    d = rs_stream_data(c->stream);
    DCHECK(d != NULL, "a byte controller's stream stopped being a ReadableStream");

    if (s->hdr.stage == BS_START) {
        int op = s->hdr.arg;
        JSValueConst a0 = s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED;

        stream_work_start(&s->w);
        byte_commit_start(&s->cm);
        s->chunk_buf = s->rethrow = JS_UNDEFINED;
        s->sub = L_NEXT;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;

        if (op == BC_ENQUEUE) {
            double off = 0, len = 0, elem = 1;
            int vt = 0;
            size_t blen = 0;
            JSValue buf = byte_view_parts(ctx, a0, &off, &len, &elem, &vt);

            if (JS_IsException(buf)) return JS_STEP_ABRUPT;
            if (!JS_GetArrayBuffer(ctx, &blen, buf)) { JS_FreeValue(ctx, buf); return JS_STEP_ABRUPT; }
            if (len == 0 || blen == 0) {
                JS_FreeValue(ctx, buf);
                JS_ThrowTypeError(ctx, "a byte stream cannot be given an empty chunk");
                return JS_STEP_ABRUPT;
            }
            if (c->close_requested) {
                JS_FreeValue(ctx, buf);
                JS_ThrowTypeError(ctx, "this stream can no longer be enqueued to");
                return JS_STEP_ABRUPT;
            }
            if (d->state != RS_READABLE) {
                JS_FreeValue(ctx, buf);
                JS_ThrowTypeError(ctx, "this stream is no longer readable");
                return JS_STEP_ABRUPT;
            }
            /* §4.9.5's ReadableByteStreamControllerEnqueue. The chunk's buffer is TRANSFERRED — the page's view
               is detached from here on, which is the whole contract of a byte stream's producer side. */
            s->chunk_buf = byte_transfer(ctx, buf);
            JS_FreeValue(ctx, buf);
            if (JS_IsException(s->chunk_buf)) { s->chunk_buf = JS_UNDEFINED; return JS_STEP_ABRUPT; }
            s->chunk_off = off;
            s->chunk_len = len;
            if (byte_pending_n(ctx, c) > 0) {
                JSValue first = byte_first_pending(ctx, c);
                JSValue fbuf = rec_obj(ctx, first, Q_BUFFER);
                size_t flen = 0;
                JSValue nb;

                if (!JS_GetArrayBuffer(ctx, &flen, fbuf)) {
                    JS_FreeValue(ctx, fbuf);
                    JS_FreeValue(ctx, first);
                    return JS_STEP_ABRUPT;
                }
                byte_invalidate_byob(ctx, c);
                nb = byte_transfer(ctx, fbuf);
                JS_FreeValue(ctx, fbuf);
                if (JS_IsException(nb)) { JS_FreeValue(ctx, first); return JS_STEP_ABRUPT; }
                JS_SetPropertyStr(ctx, first, Q_BUFFER, nb);
                if ((int)rec_num(ctx, first, D_READER) == RT_NONE &&
                    byte_enqueue_detached(ctx, c, first) < 0) {
                    JS_FreeValue(ctx, first);
                    return JS_STEP_ABRUPT;
                }
                JS_FreeValue(ctx, first);
            }
            if (!JS_IsUndefined(d->reader) && !rs_reader_data(d->reader)->byob) {
                /* ProcessReadRequestsUsingQueue, then the chunk itself */
                STEP_GOTO(s->hdr.stage, BS_DRAIN, &s->w.phase, NULL);
            } else if (!JS_IsUndefined(d->reader)) {
                if (byte_enqueue_chunk(ctx, c, s->chunk_buf, s->chunk_off, s->chunk_len) < 0) {
                    s->chunk_buf = JS_UNDEFINED;
                    return JS_STEP_ABRUPT;
                }
                s->chunk_buf = JS_UNDEFINED;
                s->cm.filled = byte_process_pending(ctx, c);
                if (JS_IsException(s->cm.filled)) { s->cm.filled = JS_UNDEFINED; return JS_STEP_ABRUPT; }
                STEP_GOTO(s->hdr.stage, BS_COMMIT, &s->w.phase, NULL);
            } else {
                DCHECK(JS_IsUndefined(d->reader), "an unlocked byte stream reached the locked enqueue arm");
                if (byte_enqueue_chunk(ctx, c, s->chunk_buf, s->chunk_off, s->chunk_len) < 0) {
                    s->chunk_buf = JS_UNDEFINED;
                    return JS_STEP_ABRUPT;
                }
                s->chunk_buf = JS_UNDEFINED;
                STEP_GOTO(s->hdr.stage, BS_PULL, &s->w.phase, NULL);
                s->w.pull = P_TEST;
            }
        } else if (op == BC_CLOSE) {
            if (c->close_requested) {
                JS_ThrowTypeError(ctx, "this stream can no longer be closed");
                return JS_STEP_ABRUPT;
            }
            if (d->state != RS_READABLE) {
                JS_ThrowTypeError(ctx, "this stream is no longer readable");
                return JS_STEP_ABRUPT;
            }
            /* §4.9.5's ReadableByteStreamControllerClose. */
            if (c->queue_total > 0) {
                c->close_requested = 1;
                return JS_STEP_DONE;
            }
            if (byte_pending_n(ctx, c) > 0) {
                JSValue first = byte_first_pending(ctx, c);
                double filled = rec_num(ctx, first, D_FILLED);
                double elem = rec_num(ctx, first, D_ELEM);
                JS_FreeValue(ctx, first);
                if (fmod(filled, elem) != 0) {
                    /* A close that would leave a partially-written ELEMENT in the consumer's view errors the
                       stream AND throws the same TypeError at whoever called close(). */
                    JS_ThrowTypeError(ctx, "this byte stream was closed with a partial element written into a "
                                           "pending pull-into");
                    s->w.err = JS_GetException(ctx);
                    s->rethrow = JS_DupValue(ctx, s->w.err);
                    byte_clear_pending(ctx, c);
                    byte_reset_queue(ctx, c);
                    readable_byte_clear_algorithms(ctx, s->hdr.this_val);
                    s->w.settle = S_ERR_SET;
                    STEP_GOTO(s->hdr.stage, BS_RETHROW, &s->w.phase, NULL);
                }
            }
            if (s->hdr.stage == BS_RETHROW) goto stages;
            readable_byte_clear_algorithms(ctx, s->hdr.this_val);
            s->w.settle = S_CLOSE_SET;
            STEP_GOTO(s->hdr.stage, BS_SETTLE, &s->w.phase, NULL);
        } else {
            DCHECK(op == BC_ERROR, "a byte controller member ran with an operation this component does not have");
            if (d->state != RS_READABLE) return JS_STEP_DONE;
            byte_clear_pending(ctx, c);
            byte_reset_queue(ctx, c);
            readable_byte_clear_algorithms(ctx, s->hdr.this_val);
            s->w.err = JS_DupValue(ctx, a0);
            s->w.settle = S_ERR_SET;
            STEP_GOTO(s->hdr.stage, BS_SETTLE, &s->w.phase, NULL);
        }
    }
stages:
    while (s->hdr.stage == BS_DRAIN) {
        /* §4.9.5's ProcessReadRequestsUsingQueue: while the reader has parked requests and the queue has
           bytes, each entry answers one — and FillReadRequestFromQueue's own HandleQueueDrain runs BETWEEN the
           dequeue and the answer, which is why the drain is a step of this loop rather than something after it. */
        if (s->sub == L_NEXT) {
            JSValue view;

            if (rs_read_pending(ctx, d) == 0 || c->queue_total == 0) break;
            view = readable_byte_take_chunk(ctx, s->hdr.this_val);
            if (JS_IsException(view)) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, s->cm.func);
            s->cm.func = rs_take_read(ctx, d, 0);
            JS_FreeValue(ctx, s->cm.value);
            s->cm.value = rs_read_result(ctx, view, false);
            if (JS_IsException(s->cm.value)) { s->cm.value = JS_UNDEFINED; return JS_STEP_ABRUPT; }
            if (readable_byte_drained(s->hdr.this_val)) {
                readable_byte_clear_algorithms(ctx, s->hdr.this_val);
                s->w.settle = S_CLOSE_SET;
            } else {
                s->w.pull = P_TEST;
            }
            s->sub = L_DRAIN;
        }
        if (s->sub == L_DRAIN) {
            r = s->w.settle != S_IDLE ? rs_settle_run(ctx, &s->w, d, cb_result, out_cb, out_argc)
                                      : readable_byte_pull_run(ctx, &s->w, s->hdr.this_val, cb_result,
                                                               out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            cb_result = JS_UNDEFINED;
            s->sub = L_CALL;
        }
        if (s->sub == L_CALL) {
            JSValueConst arg = s->cm.value;
            JSValue out;

            r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->cm.func, JS_UNDEFINED, 1, &arg,
                              cb_result, &out, out_cb, out_argc);
            if (r > 0) return r;
            if (JS_IsException(out)) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, out);
            cb_result = JS_UNDEFINED;
            s->sub = L_NEXT;
        }
    }
    if (s->hdr.stage == BS_DRAIN) {
        /* Whatever the loop left: either nobody is waiting and the chunk is queued, or a reader is and it takes
           the chunk directly — and in that second case a pull-into that was only there to receive it goes. */
        if (rs_read_pending(ctx, d) == 0) {
            DCHECK(byte_pending_n(ctx, c) == 0,
                   "a byte enqueue queued its chunk while a pull-into was still pending");
            if (byte_enqueue_chunk(ctx, c, s->chunk_buf, s->chunk_off, s->chunk_len) < 0) {
                s->chunk_buf = JS_UNDEFINED;
                return JS_STEP_ABRUPT;
            }
            s->chunk_buf = JS_UNDEFINED;
            STEP_GOTO(s->hdr.stage, BS_PULL, &s->w.phase, NULL);
            s->w.pull = P_TEST;
        } else {
            JSValue view;

            DCHECK(byte_queue_n(ctx, c) == 0, "a byte enqueue answered a reader with bytes still queued");
            if (byte_pending_n(ctx, c) > 0) {
                JSValue first = byte_first_pending(ctx, c);
                DCHECK((int)rec_num(ctx, first, D_READER) == RT_DEFAULT,
                       "a default reader's enqueue found a pull-into that is not its own");
                JS_FreeValue(ctx, first);
                JS_FreeValue(ctx, byte_shift_pending(ctx, c));
            }
            view = byte_uint8_view(ctx, s->chunk_buf, s->chunk_off, s->chunk_len);
            JS_FreeValue(ctx, s->chunk_buf);
            s->chunk_buf = JS_UNDEFINED;
            if (JS_IsException(view)) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, s->cm.func);
            s->cm.func = rs_take_read(ctx, d, 0);
            JS_FreeValue(ctx, s->cm.value);
            s->cm.value = rs_read_result(ctx, view, false);
            if (JS_IsException(s->cm.value)) { s->cm.value = JS_UNDEFINED; return JS_STEP_ABRUPT; }
            STEP_GOTO(s->hdr.stage, BS_FEED, &s->w.phase, NULL);
        }
    }

    if (s->hdr.stage == BS_FEED) {
        JSValueConst arg = s->cm.value;
        JSValue out;

        r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->cm.func, JS_UNDEFINED, 1, &arg,
                          cb_result, &out, out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, out);
        cb_result = JS_UNDEFINED;
        STEP_GOTO(s->hdr.stage, BS_PULL, &s->w.phase, NULL);
        s->w.pull = P_TEST;
    }

    if (s->hdr.stage == BS_COMMIT) {
        r = byte_commit_run(ctx, &s->w, &s->cm, d, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        STEP_GOTO(s->hdr.stage, BS_PULL, &s->w.phase, NULL);
        s->w.pull = P_TEST;
    }

    if (s->hdr.stage == BS_SETTLE) {
        r = rs_settle_run(ctx, &s->w, d, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        return JS_STEP_DONE;
    }
    if (s->hdr.stage == BS_RETHROW) {
        r = rs_settle_run(ctx, &s->w, d, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        JS_Throw(ctx, s->rethrow);
        s->rethrow = JS_UNDEFINED;
        return JS_STEP_ABRUPT;
    }

    DCHECK(s->hdr.stage == BS_PULL, "a byte controller member resumed in a stage it never parks in");
    r = readable_byte_pull_run(ctx, &s->w, s->hdr.this_val, cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    return JS_STEP_DONE;
}

#define BC_DEF(i) { sizeof(JSByteCtrlState), js_byte_ctrl_step, NULL, (i), \
                    .catches_abrupt = 1, .visit = js_byte_ctrl_visit, \
                    .algorithm = "Streams §4.7 enqueue(chunk) / close() / error(e)", .steps = BS_STEPS }
static const JSTrampStepDef js_byte_ctrl_defs[3] = {
    BC_DEF(BC_ENQUEUE), BC_DEF(BC_CLOSE), BC_DEF(BC_ERROR),
};
#undef BC_DEF

/* ---- §4.7's two ACCESSORS ---------------------------------------------------------------------------------- */

static JSValue js_bctrl_byob_request(JSContext *ctx, JSValueConst this_val, int magic)
{
    ByteCtrlData *c = bctrl_of(this_val);
    (void)magic;
    if (!c) return JS_ThrowTypeError(ctx, "not a ReadableByteStreamController");
    return byte_get_byob_request(ctx, c, this_val);
}

static JSValue js_bctrl_desired(JSContext *ctx, JSValueConst this_val, int magic)
{
    ByteCtrlData *c = bctrl_of(this_val);
    (void)magic;
    if (!c) return JS_ThrowTypeError(ctx, "not a ReadableByteStreamController");
    return byte_desired_size(ctx, c);
}

static JSValue js_byobreq_view(JSContext *ctx, JSValueConst this_val, int magic)
{
    ByobReqData *q = byobreq_of(this_val);
    (void)magic;
    if (!q) return JS_ThrowTypeError(ctx, "not a ReadableStreamBYOBRequest");
    return JS_DupValue(ctx, q->view);
}

/* ---- §4.8's respond(bytesWritten) and respondWithNewView(view) ---------------------------------------------- */

enum { BQ_RESPOND = 0, BQ_RESPOND_VIEW };

#define BQ_STAGES(X) \
    X(BQ_ENTER, "Web IDL §3.7.5 (the ReadableStreamBYOBRequest brand, before either argument is converted)") \
    X(BQ_NUM, "Web IDL §3.2.4.8 ([EnforceRange] unsigned long long bytesWritten — ToNumber is the page's code; " \
              "respondWithNewView's ArrayBufferView needs none)") \
    X(BQ_START, "Streams §4.8 respond(bytesWritten) / respondWithNewView(view) steps 1-3 and §4.9.5's " \
                "ReadableByteStreamControllerRespondInternal, up to its first run of the page's code") \
    X(BQ_COMMIT, "Streams §4.9.5 ReadableByteStreamControllerCommitPullIntoDescriptor (each filled pull-into " \
                 "answers one parked read-into request)") \
    X(BQ_SETTLE, "Streams §4.9.1 ReadableStreamError (a respond whose remainder could not be cloned errors " \
                 "the stream before it throws)") \
    X(BQ_PULL, "Streams §4.9.5 ReadableByteStreamControllerRespondInternal step 6 (CallPullIfNeeded)")
enum { BQ_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const BQ_STEPS[] = { BQ_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;
    StreamWork w;
    ByteCommit cm;
    JSValue rethrow;
    /* THE CONTROLLER, HELD BY THE MACHINE. RespondInternal's first act is to INVALIDATE the BYOB request, which
       clears the request's own pointer to its controller — so a resume that read it back off the receiver would
       find nothing, and the error path would have nothing to error. */
    JSValue ctrl;
    double n;               /* bytesWritten, held across its coercion */
} JSByobReqState;

static void js_byobreq_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSByobReqState *s = st;
    stream_work_visit(ctx, &s->w, v);
    byte_commit_visit(ctx, &s->cm, v);
    v->val(ctx, &s->rethrow);
    v->val(ctx, &s->ctrl);
}

/* §4.9.5's ReadableByteStreamControllerRespondInClosedState — every pull-into a BYOB reader is still waiting
   on is shifted and committed, with the memory returned to it and `done` true. */
static int byte_respond_closed(JSContext *ctx, ByteCtrlData *c, StreamData *d, ByteCommit *cm, JSValue first)
{
    DCHECK(fmod(rec_num(ctx, first, D_FILLED), rec_num(ctx, first, D_ELEM)) == 0,
           "a closed byte stream responded at a fill that is not a whole number of elements");
    if ((int)rec_num(ctx, first, D_READER) == RT_NONE)
        JS_FreeValue(ctx, byte_shift_pending(ctx, c));
    JS_FreeValue(ctx, first);
    if (JS_IsUndefined(d->reader) || !rs_reader_data(d->reader)->byob) return 0;
    cm->filled = JS_NewArray(ctx);
    if (JS_IsException(cm->filled)) { cm->filled = JS_UNDEFINED; return -1; }
    while (arr_len(ctx, cm->filled) < rs_read_pending(ctx, d))
        arr_push(ctx, cm->filled, byte_shift_pending(ctx, c));
    return 0;
}

/* §4.9.5's ReadableByteStreamControllerRespondInReadableState. `first` is CONSUMED. */
static int byte_respond_readable(JSContext *ctx, ByteCtrlData *c, StreamData *d, ByteCommit *cm,
                                 JSValue first, double written)
{
    double filled = rec_num(ctx, first, D_FILLED) + written;
    double elem, remainder;
    JSValue rest;

    DCHECK(filled <= rec_num(ctx, first, Q_LENGTH), "a byte stream responded past the room its pull-into had");
    DCHECK(JS_IsUndefined(c->byob_request), "a pull-into was filled while a BYOB request still named it");
    rec_set(ctx, first, D_FILLED, filled);
    if ((int)rec_num(ctx, first, D_READER) == RT_NONE) {
        int r = byte_enqueue_detached(ctx, c, first);
        JS_FreeValue(ctx, first);
        if (r < 0) return -1;
        cm->filled = byte_process_pending(ctx, c);
        if (JS_IsException(cm->filled)) { cm->filled = JS_UNDEFINED; return -1; }
        return 0;
    }
    if (filled < rec_num(ctx, first, D_MINFILL)) {
        /* A pull-into that is not yet filled up to its minimum length STAYS at the head of the list, so the
           source can keep filling it. */
        JS_FreeValue(ctx, first);
        return 0;
    }
    JS_FreeValue(ctx, byte_shift_pending(ctx, c));
    elem = rec_num(ctx, first, D_ELEM);
    remainder = fmod(filled, elem);
    if (remainder > 0) {
        JSValue buf = rec_obj(ctx, first, Q_BUFFER);
        double end = rec_num(ctx, first, Q_OFFSET) + filled;
        int r = byte_enqueue_cloned(ctx, c, buf, end - remainder, remainder);
        JS_FreeValue(ctx, buf);
        if (r < 0) { JS_FreeValue(ctx, first); return -1; }
    }
    rec_set(ctx, first, D_FILLED, filled - remainder);
    rest = byte_process_pending(ctx, c);
    if (JS_IsException(rest)) { JS_FreeValue(ctx, first); return -1; }
    cm->filled = JS_NewArray(ctx);
    if (JS_IsException(cm->filled)) {
        cm->filled = JS_UNDEFINED;
        JS_FreeValue(ctx, rest);
        JS_FreeValue(ctx, first);
        return -1;
    }
    /* The descriptor this respond FILLED is committed first, and the ones the leftover bytes filled after it. */
    arr_push(ctx, cm->filled, first);
    {
        uint32_t i, n = arr_len(ctx, rest);
        for (i = 0; i < n; i++) arr_push(ctx, cm->filled, JS_GetPropertyUint32(ctx, rest, i));
    }
    JS_FreeValue(ctx, rest);
    return 0;
}

/* §4.9.5's ReadableByteStreamControllerRespondInternal. Returns 0, or -1 with the exception live — which the
   caller ERRORS the stream with before re-raising it. */
static int byte_respond_internal(JSContext *ctx, ByteCtrlData *c, StreamData *d, ByteCommit *cm, double written)
{
    JSValue first;

    byte_invalidate_byob(ctx, c);
    first = byte_first_pending(ctx, c);
    if (d->state == RS_CLOSED) {
        DCHECK(written == 0, "a closed byte stream was told bytes had been written into its pull-into");
        return byte_respond_closed(ctx, c, d, cm, first);
    }
    DCHECK(d->state == RS_READABLE, "a byte stream responded to a pull-into while it was errored");
    DCHECK(written > 0, "a readable byte stream was told that nothing had been written into its pull-into");
    return byte_respond_readable(ctx, c, d, cm, first, written);
}

static int js_byobreq_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSByobReqState *s = st;
    ByobReqData *q = byobreq_of(s->hdr.this_val);
    ByteCtrlData *c;
    StreamData *d;
    int r;

    if (!q) {
        JS_FreeValue(ctx, cb_result);
        JS_ThrowTypeError(ctx, "not a ReadableStreamBYOBRequest");
        return JS_STEP_ABRUPT;
    }

    if (s->hdr.stage == BQ_ENTER) {
        stream_work_start(&s->w);
        byte_commit_start(&s->cm);
        s->rethrow = s->ctrl = JS_UNDEFINED;
        s->n = 0;
        s->hdr.stage = BQ_NUM;
    }

    if (s->hdr.stage == BQ_NUM) {
        if (s->hdr.arg == BQ_RESPOND) {
            /* Web IDL converts the argument BEFORE the method's own steps, and `[EnforceRange] unsigned long
               long` is ToNumber (the page's `valueOf`) and then §3.2.4.8's range — a fractional, negative,
               non-finite or too-large value is a TypeError, never a silent clamp. */
            double x = 0;
            r = step_todouble_run(ctx, &s->hdr, s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED,
                                  cb_result, &x, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            cb_result = JS_UNDEFINED;
            if (!isfinite(x) || x != trunc(x) || x < 0 || x > 18446744073709551615.0) {
                JS_ThrowTypeError(ctx, "bytesWritten is outside the range of an unsigned long long");
                return JS_STEP_ABRUPT;
            }
            s->n = x;
        }
        s->hdr.stage = BQ_START;
    }

    if (s->hdr.stage == BQ_START) {
        JSValue first;
        double filled, blen, off, elem;
        int vt;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (JS_IsUndefined(q->controller)) {
            JS_ThrowTypeError(ctx, "this BYOB request has already been responded to");
            return JS_STEP_ABRUPT;
        }
        s->ctrl = JS_DupValue(ctx, q->controller);
        c = bctrl_of(s->ctrl);
        DCHECK(c != NULL, "a BYOB request held something that is not a ReadableByteStreamController");
        d = rs_stream_data(c->stream);
        DCHECK(d != NULL, "a byte controller's stream stopped being a ReadableStream");
        DCHECK(byte_pending_n(ctx, c) > 0, "a BYOB request outlived the pull-into it was made for");

        if (s->hdr.arg == BQ_RESPOND) {
            /* §4.8 step 2: the view's buffer must still be there — the source may have transferred it away
               between being handed the request and answering it. */
            JSValue vb = byte_view_parts(ctx, q->view, &off, &blen, &elem, &vt);
            if (JS_IsException(vb)) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, vb);
            DCHECK(blen > 0, "a BYOB request was made over an empty view");
            first = byte_first_pending(ctx, c);
            filled = rec_num(ctx, first, D_FILLED);
            if (d->state == RS_CLOSED) {
                if (s->n != 0) {
                    JS_FreeValue(ctx, first);
                    JS_ThrowTypeError(ctx, "a closed byte stream can only be responded to with 0 bytes");
                    return JS_STEP_ABRUPT;
                }
            } else {
                DCHECK(d->state == RS_READABLE, "a BYOB request was responded to on an errored stream");
                if (s->n == 0) {
                    JS_FreeValue(ctx, first);
                    JS_ThrowTypeError(ctx, "a readable byte stream cannot be responded to with 0 bytes");
                    return JS_STEP_ABRUPT;
                }
                if (filled + s->n > rec_num(ctx, first, Q_LENGTH)) {
                    JS_FreeValue(ctx, first);
                    JS_ThrowRangeError(ctx, "more bytes were written than the pull-into had room for");
                    return JS_STEP_ABRUPT;
                }
            }
            {
                JSValue buf = rec_obj(ctx, first, Q_BUFFER);
                JSValue nb = byte_transfer(ctx, buf);
                JS_FreeValue(ctx, buf);
                if (JS_IsException(nb)) { JS_FreeValue(ctx, first); return JS_STEP_ABRUPT; }
                JS_SetPropertyStr(ctx, first, Q_BUFFER, nb);
            }
            JS_FreeValue(ctx, first);
        } else {
            /* §4.9.5's ReadableByteStreamControllerRespondWithNewView: the source wrote into a DIFFERENT view
               of the same memory, so the descriptor adopts that view's buffer and its length is what was
               written. */
            JSValueConst nv = s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED;
            JSValue vb = byte_view_parts(ctx, nv, &off, &blen, &elem, &vt);
            size_t vbuflen = 0;

            if (JS_IsException(vb)) return JS_STEP_ABRUPT;
            if (!JS_GetArrayBuffer(ctx, &vbuflen, vb)) { JS_FreeValue(ctx, vb); return JS_STEP_ABRUPT; }
            first = byte_first_pending(ctx, c);
            filled = rec_num(ctx, first, D_FILLED);
            if (d->state == RS_CLOSED) {
                if (blen != 0) goto view_type_error;
            } else {
                DCHECK(d->state == RS_READABLE, "a BYOB request was responded to on an errored stream");
                if (blen == 0) goto view_type_error;
            }
            if (rec_num(ctx, first, Q_OFFSET) + filled != off ||
                rec_num(ctx, first, D_BUFLEN) != (double)vbuflen ||
                filled + blen > rec_num(ctx, first, Q_LENGTH)) {
                JS_FreeValue(ctx, first);
                JS_FreeValue(ctx, vb);
                JS_ThrowRangeError(ctx, "the view responded with does not describe the region the pull-into "
                                        "asked to be filled");
                return JS_STEP_ABRUPT;
            }
            {
                JSValue nb = byte_transfer(ctx, vb);
                JS_FreeValue(ctx, vb);
                if (JS_IsException(nb)) { JS_FreeValue(ctx, first); return JS_STEP_ABRUPT; }
                JS_SetPropertyStr(ctx, first, Q_BUFFER, nb);
            }
            JS_FreeValue(ctx, first);
            s->n = blen;
            goto responded;
        view_type_error:
            JS_FreeValue(ctx, first);
            JS_FreeValue(ctx, vb);
            JS_ThrowTypeError(ctx, d->state == RS_CLOSED
                                   ? "a closed byte stream can only be responded to with an empty view"
                                   : "a readable byte stream cannot be responded to with an empty view");
            return JS_STEP_ABRUPT;
        }
    responded:
        if (byte_respond_internal(ctx, c, d, &s->cm, s->n) < 0) {
            /* The one failure §4.9.5 recovers from: the remainder could not be cloned, so the controller is
               ERRORED with that exception and the member re-raises it. */
            s->w.err = JS_GetException(ctx);
            s->rethrow = JS_DupValue(ctx, s->w.err);
            byte_clear_pending(ctx, c);
            byte_reset_queue(ctx, c);
            readable_byte_clear_algorithms(ctx, s->ctrl);
            s->w.settle = S_ERR_SET;
            s->hdr.stage = BQ_SETTLE;
        } else {
            s->hdr.stage = BQ_COMMIT;
        }
    }

    c = bctrl_of(s->ctrl);
    DCHECK(c != NULL, "a BYOB request member resumed without the controller it had responded to");
    d = rs_stream_data(c->stream);
    DCHECK(d != NULL, "a byte controller's stream stopped being a ReadableStream");

    if (s->hdr.stage == BQ_COMMIT) {
        r = byte_commit_run(ctx, &s->w, &s->cm, d, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        s->hdr.stage = BQ_PULL;
        s->w.pull = P_TEST;
    }
    if (s->hdr.stage == BQ_SETTLE) {
        r = rs_settle_run(ctx, &s->w, d, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        JS_Throw(ctx, s->rethrow);
        s->rethrow = JS_UNDEFINED;
        return JS_STEP_ABRUPT;
    }

    DCHECK(s->hdr.stage == BQ_PULL, "a BYOB request member resumed in a stage it never parks in");
    r = readable_byte_pull_run(ctx, &s->w, s->ctrl, cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    return JS_STEP_DONE;
}

#define BQ_DEF(i) { sizeof(JSByobReqState), js_byobreq_step, NULL, (i), \
                    .catches_abrupt = 1, .visit = js_byobreq_visit, \
                    .algorithm = "Streams §4.8 respond(bytesWritten) / respondWithNewView(view)", \
                    .steps = BQ_STEPS }
static const JSTrampStepDef js_byobreq_defs[2] = { BQ_DEF(BQ_RESPOND), BQ_DEF(BQ_RESPOND_VIEW) };
#undef BQ_DEF

/* ---- §4.5's read(view, options) ----------------------------------------------------------------------------- */

#define BR_STAGES(X) \
    X(BR_START, "Streams §4.5 read(view, options) — Web IDL §3.7.5's brand and §3.2.25's ArrayBufferView, " \
                "before the dictionary is read") \
    X(BR_MIN, "Web IDL §3.2.17 (ReadableStreamBYOBReaderReadOptions[\"min\"] — the [[Get]] is the page's code)") \
    X(BR_MINNUM, "Web IDL §3.2.4.8 ([EnforceRange] unsigned long long min — ToNumber is the page's code)") \
    X(BR_INTO, "Streams §4.5 read() steps 1-11 and §4.9.5's ReadableByteStreamControllerPullInto") \
    X(BR_EMPTYVIEW, "Streams §4.9.5 ReadableByteStreamControllerPullInto's closed branch (constructing the " \
                    "EMPTY view that gives the caller its memory back — %DataView% is a step machine)") \
    X(BR_QUEUEVIEW, "Streams §4.9.5 ReadableByteStreamControllerConvertPullIntoDescriptor, for a pull-into the " \
                    "QUEUE filled outright (the same construct, over the bytes that were written)") \
    X(BR_DRAIN, "Streams §4.9.5 ReadableByteStreamControllerHandleQueueDrain / ReadableByteStreamControllerError " \
                "(what the pull-into's outcome does to the stream before the promise is settled)") \
    X(BR_SETTLE, "Streams §4.5 read()'s read-into request steps (its chunk, close or error steps — settling " \
                 "the promise is the page's code)")
enum { BR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const BR_STEPS[] = { BR_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;
    StreamWork w;
    JSValue promise;   /* the capability handed back to the page */
    JSValue settle;    /* its resolve or reject function, or JS_UNDEFINED when the request PARKED */
    JSValue result;    /* what to settle it with */
    JSValue stream;
    /* THE PULL-INTO DESCRIPTOR A VIEW IS BEING BUILT FROM, and the state that construction parks on. §4.9.5's
       PullInto reaches its view constructor twice — the closed branch's EMPTY view and the queue-filled
       branch's Convert — and both name the descriptor's own type, so a read whose view is a DataView
       constructs a step machine and suspends. The two branches are mutually exclusive within one read, which
       is why they share one phase byte and one buffer rather than carrying two of each. */
    JSValue dsc;
    ByteViewCb view_cb;
    double  min;
    uint8_t view_phase;
} JSByobReadState;

static void js_byob_read_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSByobReadState *s = st;
    int i;
    stream_work_visit(ctx, &s->w, v);
    v->val(ctx, &s->promise);
    v->val(ctx, &s->settle);
    v->val(ctx, &s->result);
    v->val(ctx, &s->stream);
    v->val(ctx, &s->dsc);
    STEP_CB_FOREACH(s->view_cb, i) v->val(ctx, &s->view_cb[i]);
}

static JSValue js_byob_read_fini(JSContext *ctx, void *st, bool take_result)
{
    JSByobReadState *s = st;
    JSValue r = take_result ? s->promise : JS_UNDEFINED;
    (void)ctx;
    if (take_result) s->promise = JS_UNDEFINED;
    return r;
}

/* EVERY refusal in `read` is a REJECTED PROMISE and not a throw — Web IDL §3.7.6 converts an exception thrown
   by a promise-returning operation into one, and §4.5's own steps state it directly. */
static int byob_read_reject(JSContext *ctx, JSByobReadState *s)
{
    JSValue funcs[2];

    JS_FreeValue(ctx, s->result);   /* the raw `min` the coercion was reading, when that is what threw */
    s->result = JS_GetException(ctx);
    DCHECK(JS_IsUndefined(s->promise), "a BYOB read rejected after it had already made its promise");
    s->promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(s->promise)) return -1;
    s->settle = funcs[1];
    JS_FreeValue(ctx, funcs[0]);
    s->hdr.stage = BR_SETTLE;
    return 0;
}

static int js_byob_read_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSByobReadState *s = st;
    JSValueConst view = s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED;
    JSValueConst opts = s->hdr.argc > 1 ? s->hdr.argv[1] : JS_UNDEFINED;
    JSValueConst arg;
    JSValue out;
    int r;

    if (s->hdr.stage == BR_START) {
        ReaderData *rd = rs_reader_data(s->hdr.this_val);
        double off = 0, len = 0, elem = 1;
        int vt = 0, i;
        JSValue buf;

        stream_work_start(&s->w);
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* EVERY OWNED FIELD IS PLACED BEFORE THE FIRST OPERATION THAT CAN THROW, because the failure path tears
           this state down through fini, which frees exactly what the state holds and nothing else. */
        s->promise = s->settle = s->result = s->stream = s->dsc = JS_UNDEFINED;
        STEP_CB_FOREACH(s->view_cb, i) s->view_cb[i] = JS_UNDEFINED;
        s->view_phase = 0;
        s->min = 1;
        if (!rd || !rd->byob) {
            JS_ThrowTypeError(ctx, "not a ReadableStreamBYOBReader");
            { if (byob_read_reject(ctx, s) < 0) return JS_STEP_ABRUPT; goto settle; }
        }
        /* §3.2.25's ArrayBufferView conversion, which is also §4.5 step 3's detached test: a view whose buffer
           has gone answers the same TypeError. */
        buf = byte_view_parts(ctx, view, &off, &len, &elem, &vt);
        if (JS_IsException(buf)) { if (byob_read_reject(ctx, s) < 0) return JS_STEP_ABRUPT; goto settle; }
        JS_FreeValue(ctx, buf);
        STEP_GOTO(s->hdr.stage, BR_MIN, &s->w.phase, &s->view_phase, &s->hdr.get_phase, NULL);
    }

    if (s->hdr.stage == BR_MIN) {
        /* Web IDL §3.2.17's dictionary conversion, which is a [[Get]] of `min` and then §3.2.4.8's coercion — both the
           page's code, and both BEFORE §4.5's own steps 1-8. */
        if (JS_IsUndefined(opts) || JS_IsNull(opts)) {
            STEP_GOTO(s->hdr.stage, BR_INTO, &s->w.phase, &s->view_phase, &s->hdr.get_phase, NULL);
        } else if (!JS_IsObject(opts)) {
            JS_ThrowTypeError(ctx, "the read options must be an object");
            { if (byob_read_reject(ctx, s) < 0) return JS_STEP_ABRUPT; goto settle; }
        } else {
            JSAtom a = JS_NewAtom(ctx, "min");
            JSValue got = JS_UNDEFINED;
            r = step_getprop_run(ctx, &s->hdr, opts, a, cb_result, &got, out_cb, out_argc);
            JS_FreeAtom(ctx, a);
            if (r > 0) return r;
            if (r < 0) { if (byob_read_reject(ctx, s) < 0) return JS_STEP_ABRUPT; goto settle; }
            cb_result = JS_UNDEFINED;
            JS_FreeValue(ctx, s->result);
            s->result = got;
            STEP_GOTO(s->hdr.stage, JS_IsUndefined(got) ? BR_INTO : BR_MINNUM, &s->w.phase, &s->view_phase,
                      &s->hdr.get_phase, NULL);
        }
    }

    if (s->hdr.stage == BR_MINNUM) {
        double x = 0;
        r = step_todouble_run(ctx, &s->hdr, s->result, cb_result, &x, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) { if (byob_read_reject(ctx, s) < 0) return JS_STEP_ABRUPT; goto settle; }
        cb_result = JS_UNDEFINED;
        JS_FreeValue(ctx, s->result);
        s->result = JS_UNDEFINED;
        if (!isfinite(x) || x != trunc(x) || x < 0 || x > 18446744073709551615.0) {
            JS_ThrowTypeError(ctx, "min is outside the range of an unsigned long long");
            { if (byob_read_reject(ctx, s) < 0) return JS_STEP_ABRUPT; goto settle; }
        }
        s->min = x;
        STEP_GOTO(s->hdr.stage, BR_INTO, &s->w.phase, &s->view_phase, &s->hdr.get_phase, NULL);
    }

    if (s->hdr.stage == BR_INTO) {
        ReaderData *rd = rs_reader_data(s->hdr.this_val);
        StreamData *d;
        ByteCtrlData *c;
        JSValue funcs[2], buf, nb, dsc;
        double off = 0, len = 0, elem = 1, minfill;
        int vt = 0;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        DCHECK(rd != NULL && rd->byob, "a BYOB read resumed on something that is not a BYOB reader");
        buf = byte_view_parts(ctx, view, &off, &len, &elem, &vt);
        if (JS_IsException(buf)) { if (byob_read_reject(ctx, s) < 0) return JS_STEP_ABRUPT; goto settle; }
        {
            size_t blen = 0;
            if (!JS_GetArrayBuffer(ctx, &blen, buf)) {
                JS_FreeValue(ctx, buf);
                { if (byob_read_reject(ctx, s) < 0) return JS_STEP_ABRUPT; goto settle; }
            }
            if (len == 0 || blen == 0) {
                JS_FreeValue(ctx, buf);
                JS_ThrowTypeError(ctx, "a BYOB read cannot be given an empty view");
                { if (byob_read_reject(ctx, s) < 0) return JS_STEP_ABRUPT; goto settle; }
            }
        }
        if (s->min == 0) {
            JS_FreeValue(ctx, buf);
            JS_ThrowTypeError(ctx, "a BYOB read's min must not be 0");
            { if (byob_read_reject(ctx, s) < 0) return JS_STEP_ABRUPT; goto settle; }
        }
        if (s->min * elem > len) {
            JS_FreeValue(ctx, buf);
            JS_ThrowRangeError(ctx, "a BYOB read's min is larger than the view it was given");
            { if (byob_read_reject(ctx, s) < 0) return JS_STEP_ABRUPT; goto settle; }
        }
        if (JS_IsUndefined(rd->stream)) {
            JS_FreeValue(ctx, buf);
            JS_ThrowTypeError(ctx, "this reader has been released");
            { if (byob_read_reject(ctx, s) < 0) return JS_STEP_ABRUPT; goto settle; }
        }
        s->promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(s->promise)) { JS_FreeValue(ctx, buf); return JS_STEP_ABRUPT; }
        s->stream = JS_DupValue(ctx, rd->stream);
        d = rs_stream_data(s->stream);
        DCHECK(d != NULL, "a reader's stream stopped being a ReadableStream while it held the lock");
        c = bctrl_of(d->controller);
        DCHECK(c != NULL, "a BYOB reader was holding a stream whose controller is not a byte one");
        /* §4.9.3's ReadableStreamBYOBReaderRead. */
        d->disturbed = 1;
        if (d->state == RS_ERRORED) {
            JS_FreeValue(ctx, buf);
            s->result = JS_DupValue(ctx, d->stored_error);
            s->settle = funcs[1];
            JS_FreeValue(ctx, funcs[0]);
            STEP_GOTO(s->hdr.stage, BR_SETTLE, &s->w.phase, &s->view_phase, &s->hdr.get_phase, NULL);
            goto into_done;
        }
        /* §4.9.5's ReadableByteStreamControllerPullInto. */
        minfill = s->min * elem;
        nb = byte_transfer(ctx, buf);
        JS_FreeValue(ctx, buf);
        if (JS_IsException(nb)) {
            s->result = JS_GetException(ctx);
            s->settle = funcs[1];
            JS_FreeValue(ctx, funcs[0]);
            STEP_GOTO(s->hdr.stage, BR_SETTLE, &s->w.phase, &s->view_phase, &s->hdr.get_phase, NULL);
            goto into_done;
        }
        dsc = JS_NewObjectProto(ctx, JS_NULL);
        if (JS_IsException(dsc)) {
            JS_FreeValue(ctx, nb);
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            return JS_STEP_ABRUPT;
        }
        {
            size_t nblen = 0;
            uint8_t *np = JS_GetArrayBuffer(ctx, &nblen, nb);
            CHECK(np != NULL, "a freshly transferred buffer was already detached");
            JS_DefinePropertyValueStr(ctx, dsc, D_BUFLEN, JS_NewFloat64(ctx, (double)nblen), JS_PROP_C_W_E);
        }
        JS_DefinePropertyValueStr(ctx, dsc, Q_BUFFER, nb, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, dsc, Q_OFFSET, JS_NewFloat64(ctx, off), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, dsc, Q_LENGTH, JS_NewFloat64(ctx, len), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, dsc, D_FILLED, JS_NewInt32(ctx, 0), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, dsc, D_MINFILL, JS_NewFloat64(ctx, minfill), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, dsc, D_ELEM, JS_NewFloat64(ctx, elem), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, dsc, D_VIEW, JS_NewInt32(ctx, vt), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, dsc, D_READER, JS_NewInt32(ctx, RT_BYOB), JS_PROP_C_W_E);

        if (byte_pending_n(ctx, c) > 0) {
            /* Somebody is already being filled: this one goes behind it and NOTHING is pulled, because the
               source is already working on the head of the list. */
            arr_push(ctx, c->pending, dsc);
            rs_park_read(ctx, d, funcs);
            STEP_GOTO(s->hdr.stage, BR_SETTLE, &s->w.phase, &s->view_phase, &s->hdr.get_phase, NULL);
            goto into_done;
        }
        if (d->state == RS_CLOSED) {
            /* The close steps take a view so the BACKING MEMORY goes back to the caller — and the view is of
               the descriptor's own type, so this construction is a REQUEST and its stage is its own. The
               descriptor and the capability are handed to the state BEFORE the park, because from here on this
               machine's teardown is what frees them. */
            s->dsc = dsc;
            s->settle = funcs[0];
            JS_FreeValue(ctx, funcs[1]);
            STEP_GOTO(s->hdr.stage, BR_EMPTYVIEW, &s->w.phase, &s->view_phase, &s->hdr.get_phase, NULL);
            goto into_done;
        }
        if (c->queue_total > 0) {
            bool ready = false;
            if (byte_fill_from_queue(ctx, c, dsc, &ready) < 0) {
                JS_FreeValue(ctx, dsc);
                JS_FreeValue(ctx, funcs[0]);
                JS_FreeValue(ctx, funcs[1]);
                return JS_STEP_ABRUPT;
            }
            if (ready) {
                s->dsc = dsc;
                s->settle = funcs[0];
                JS_FreeValue(ctx, funcs[1]);
                STEP_GOTO(s->hdr.stage, BR_QUEUEVIEW, &s->w.phase, &s->view_phase, &s->hdr.get_phase, NULL);
                goto into_done;
            }
            if (c->close_requested) {
                JS_FreeValue(ctx, dsc);
                JS_ThrowTypeError(ctx, "this byte stream closed with a partial element left in the queue");
                s->w.err = JS_GetException(ctx);
                s->result = JS_DupValue(ctx, s->w.err);
                s->settle = funcs[1];
                JS_FreeValue(ctx, funcs[0]);
                byte_clear_pending(ctx, c);
                byte_reset_queue(ctx, c);
                readable_byte_clear_algorithms(ctx, d->controller);
                s->w.settle = S_ERR_SET;
                STEP_GOTO(s->hdr.stage, BR_DRAIN, &s->w.phase, &s->view_phase, &s->hdr.get_phase, NULL);
                goto into_done;
            }
        }
        arr_push(ctx, c->pending, dsc);
        rs_park_read(ctx, d, funcs);
        s->w.pull = P_TEST;
        STEP_GOTO(s->hdr.stage, BR_DRAIN, &s->w.phase, &s->view_phase, &s->hdr.get_phase, NULL);
    into_done:;
    }

    if (s->hdr.stage == BR_EMPTYVIEW) {
        JSValue empty = JS_UNDEFINED;

        r = byte_empty_view_run(ctx, &s->view_phase, STEP_CB(s->view_cb), s->dsc, cb_result, &empty,
                                out_cb, out_argc);
        if (r > 0) return r;
        cb_result = JS_UNDEFINED;
        JS_FreeValue(ctx, s->dsc);
        s->dsc = JS_UNDEFINED;
        if (JS_IsException(empty)) return JS_STEP_ABRUPT;
        s->result = rs_read_result(ctx, empty, true);
        if (JS_IsException(s->result)) { s->result = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        STEP_GOTO(s->hdr.stage, BR_SETTLE, &s->w.phase, &s->view_phase, &s->hdr.get_phase, NULL);
    }

    if (s->hdr.stage == BR_QUEUEVIEW) {
        StreamData *qd = rs_stream_data(s->stream);
        JSValue filled_view = JS_UNDEFINED;

        DCHECK(qd != NULL, "the BYOB read machine's stream stopped being a ReadableStream");
        r = byte_convert_pull_into_run(ctx, &s->view_phase, STEP_CB(s->view_cb), s->dsc, cb_result,
                                       &filled_view, out_cb, out_argc);
        if (r > 0) return r;
        cb_result = JS_UNDEFINED;
        JS_FreeValue(ctx, s->dsc);
        s->dsc = JS_UNDEFINED;
        if (JS_IsException(filled_view)) return JS_STEP_ABRUPT;
        s->result = rs_read_result(ctx, filled_view, false);
        if (JS_IsException(s->result)) { s->result = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        /* HandleQueueDrain, BEFORE the chunk steps. */
        if (readable_byte_drained(qd->controller)) {
            readable_byte_clear_algorithms(ctx, qd->controller);
            s->w.settle = S_CLOSE_SET;
        } else {
            s->w.pull = P_TEST;
        }
        STEP_GOTO(s->hdr.stage, BR_DRAIN, &s->w.phase, &s->view_phase, &s->hdr.get_phase, NULL);
    }

    if (s->hdr.stage == BR_DRAIN) {
        StreamData *d = rs_stream_data(s->stream);
        DCHECK(d != NULL, "the BYOB read machine's stream stopped being a ReadableStream");
        r = s->w.settle != S_IDLE ? rs_settle_run(ctx, &s->w, d, cb_result, out_cb, out_argc)
                                  : readable_byte_pull_run(ctx, &s->w, d->controller, cb_result,
                                                           out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        STEP_GOTO(s->hdr.stage, BR_SETTLE, &s->w.phase, &s->view_phase, &s->hdr.get_phase, NULL);
    }

settle:
    DCHECK(s->hdr.stage == BR_SETTLE, "the BYOB read machine resumed in a stage it never parks in");
    if (JS_IsUndefined(s->settle)) {
        /* the request parked: the controller owns the answer now */
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

static const JSTrampStepDef js_byob_read_def = {
    sizeof(JSByobReadState), js_byob_read_step, js_byob_read_fini, 0, .catches_abrupt = 1,
    .visit = js_byob_read_visit,
    .algorithm = "Streams §4.5 read(view, options), through §4.9.5 ReadableByteStreamControllerPullInto",
    .steps = BR_STEPS
};

int readable_byob_read_stepid(void)
{
    return g_byob_read_stepid;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

void readable_byte_stream_init(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSClassDef cd = { "ReadableByteStreamController", .finalizer = bctrl_finalizer, .gc_mark = bctrl_gc_mark };
    JSClassDef qd = { "ReadableStreamBYOBRequest", .finalizer = byobreq_finalizer, .gc_mark = byobreq_gc_mark };
    int i;

    DCHECK(g_bs_rt == NULL || g_bs_rt == rt, "ReadableByteStreamController was installed into a second runtime");
    if (g_bs_rt == rt) return;
    g_bs_rt = rt;
    JS_NewClassID(rt, &g_bctrl_class);
    JS_NewClass(rt, g_bctrl_class, &cd);
    JS_NewClassID(rt, &g_byobreq_class);
    JS_NewClass(rt, g_byobreq_class, &qd);

    for (i = 0; i < 3; i++) {
        g_bctrl_stepids[i] = JS_RegisterStepDef(rt, &js_byte_ctrl_defs[i]);
        CHECK(g_bctrl_stepids[i] >= 0, "streams: no step id for a byte controller member");
    }
    for (i = 0; i < 2; i++) {
        g_byobreq_stepids[i] = JS_RegisterStepDef(rt, &js_byobreq_defs[i]);
        CHECK(g_byobreq_stepids[i] >= 0, "streams: no step id for a BYOB request member");
    }
    for (i = 0; i < 4; i++) {
        g_byte_rxn_stepids[i] = JS_RegisterStepDef(rt, &js_byte_rxn_defs[i]);
        CHECK(g_byte_rxn_stepids[i] >= 0, "streams: no step id for a §4.9.5 reaction");
    }
    g_byob_read_stepid = JS_RegisterStepDef(rt, &js_byob_read_def);
    CHECK(g_byob_read_stepid >= 0, "streams: no step id for the BYOB reader's read");
}

void readable_byte_stream_install_protos(JSContext *ctx)
{
    static const char *const BC_NAMES[3] = { "enqueue", "close", "error" };
    static const char *const BQ_NAMES[2] = { "respond", "respondWithNewView" };
    JSValue ctrl_p, req_p, prev;
    int i;

    DCHECK(g_bctrl_class != 0, "a realm asked for ReadableByteStreamController.prototype before its class");
    prev = JS_GetClassProto(ctx, g_bctrl_class);
    DCHECK(JS_IsNull(prev), "readable_byte_stream_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);

    ctrl_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(ctrl_p), "ReadableByteStreamController.prototype could not be allocated");
    idl_interface_tag(ctx, ctrl_p, "ReadableByteStreamController");
    idl_install_accessor(ctx, ctrl_p, "byobRequest", js_bctrl_byob_request, 0, -1);
    idl_install_accessor(ctx, ctrl_p, "desiredSize", js_bctrl_desired, 0, -1);
    for (i = 0; i < 3; i++)
        idl_install_step_method(ctx, ctrl_p, BC_NAMES[i], i == 0 ? 1 : 0, g_bctrl_stepids[i]);
    JS_SetClassProto(ctx, g_bctrl_class, ctrl_p);

    req_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(req_p), "ReadableStreamBYOBRequest.prototype could not be allocated");
    idl_interface_tag(ctx, req_p, "ReadableStreamBYOBRequest");
    idl_install_accessor(ctx, req_p, "view", js_byobreq_view, 0, -1);
    for (i = 0; i < 2; i++)
        idl_install_step_method(ctx, req_p, BQ_NAMES[i], 1, g_byobreq_stepids[i]);
    JS_SetClassProto(ctx, g_byobreq_class, req_p);
}

void readable_byte_stream_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto, ctor;

    proto = JS_GetClassProto(ctx, g_bctrl_class);
    DCHECK(!JS_IsNull(proto), "ReadableByteStreamController was installed into a realm with no proto build");
    ctor = idl_interface_object(ctx, "ReadableByteStreamController", proto);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(ctor), "the byte controller interface object could not be allocated");
    JS_SetPropertyStr(ctx, (JSValue)global, "ReadableByteStreamController", ctor);

    proto = JS_GetClassProto(ctx, g_byobreq_class);
    DCHECK(!JS_IsNull(proto), "ReadableStreamBYOBRequest was installed into a realm with no proto build");
    ctor = idl_interface_object(ctx, "ReadableStreamBYOBRequest", proto);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(ctor), "the BYOB request interface object could not be allocated");
    JS_SetPropertyStr(ctx, (JSValue)global, "ReadableStreamBYOBRequest", ctor);
}

void readable_byte_stream_free(void)
{
    int i;
    if (!g_bs_rt) return;
    g_bs_rt = NULL;
    for (i = 0; i < 3; i++) g_bctrl_stepids[i] = -1;
    for (i = 0; i < 2; i++) g_byobreq_stepids[i] = -1;
    for (i = 0; i < 4; i++) g_byte_rxn_stepids[i] = -1;
    g_byob_read_stepid = -1;
}
