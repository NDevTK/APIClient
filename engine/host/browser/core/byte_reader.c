/* READING A BYTE SEQUENCE AS A PROMISE — Fetch §5.2's body readers and File API §3.3's Blob readers.
 *
 * WHY IT IS NOT IN body.c, WHERE IT WAS WRITTEN. `blob.text()`, `blob.arrayBuffer()` and `blob.bytes()` are the
 * same three algorithms as `response.text()`, `response.arrayBuffer()` and `response.bytes()` — File API defines
 * them by the same "read all bytes" the Fetch body consumption is defined by. Leaving the machine under
 * core/fetch/ would have made File API depend on Fetch, and the dependency runs the OTHER way: BodyInit's union
 * names Blob, and Fetch's `blob()` reader returns one. So the machine lives where both can reach it, and each
 * spec declares its own readers into it.
 *
 * WHAT EACH SPEC KEEPS. The single-use latch is Fetch's, because a body is a stream; a Blob is an immutable byte
 * sequence and re-reads. `json()` and `formData()` are Fetch's members and not Blob's. Both facts arrive here as
 * an interface's own `take` and its own reader table, so neither is a condition this file tests.
 *
 * THE BYTES ARE ALREADY HERE, so the promise is settled before the page ever sees it — but SETTLING it is not a
 * C-private act. 27.2.1.3.2 step 8 reads `Get(resolution, "then")` off the value, which for `json()`'s result is
 * an ordinary object whose prototype the page owns: `Object.prototype.then = { get(){…} }` makes that read the
 * page's code, and prototype pollution is a gadget class this engine exists to run rather than assume away.
 * Performed with a JS_Call from C it would run in an activation with no flow base, so a loop in that getter
 * would drive to completion. The resolving function is a CALL REQUEST instead, and the read happens on the tramp
 * where it can suspend and fork. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/byte_reader.h"
#include "core/streams/readable_stream.h"

/* HOW MANY READERS ONE INTERFACE CAN DECLARE. The step defs are STATIC initialisers, one per reader INDEX,
   because engine/check_step_visits.mjs reads them out of the source — a def assembled at runtime is invisible to
   the gate that pairs every declaration with the visit for its state struct, and four of these silently stopped
   being covered the last time one was built that way. Fetch declares five and File API three, so the ceiling is
   a DCHECK on a fixed platform rather than a growth path nobody exercises. */
#define BYTE_READER_MAX 8
#define BYTE_READER_IFACE_MAX 4

static const ByteReaderIface *g_iface[BYTE_READER_IFACE_MAX];
static int g_iface_stepid[BYTE_READER_IFACE_MAX][BYTE_READER_MAX];
static int g_iface_n;

/* WHICH INTERFACE THE RECEIVER BELONGS TO is found from the receiver, not carried in the def's `arg` — `arg` is
   the reader INDEX, which is what keeps the defs static. */
static const ByteReaderIface *iface_of(JSValueConst v)
{
    int i;
    for (i = 0; i < g_iface_n; i++)
        if (g_iface[i]->is(v)) return g_iface[i];
    return NULL;
}

/* A BODY THAT IS A STREAM takes two more stages before there are any bytes: acquire a reader and issue the
   first read. Both are CALLS, which is precisely why they are here — this is a machine and can park on them —
   and the LOOP after them is not, which is why readable_stream_drain owns that. */
enum { BR_TAKE = 0, BR_ACQUIRE, BR_FIRST_READ, BR_SETTLE };

typedef struct JSByteReaderState {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   stage;
    uint8_t   cphase;   /* the settle call's own phase, so the stage can hold it across a suspension */
    JSValue   promise;  /* the capability's promise — this machine's result (owned) */
    JSValue   func;     /* its resolve or its reject, whichever this read settles with (owned) */
    JSValue   value;    /* what it settles WITH: the text, the parsed body, or the error (owned) */
    JSValue   stream;   /* the body's stream, when the body IS one (owned) */
    JSValue   reader;   /* the reader acquired on it (owned) */
    JSValue   cb[3];    /* the call request buffer: [this, resolving function, value] */
} JSByteReaderState;

/* WHAT THIS MACHINE OWNS. The call buffer is in here for the reason dispatch's is: a `then` getter that forks
   the flow must not leave two arms sharing one invocation. */
static void js_byte_reader_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSByteReaderState *s = st;
    int k;
    v->val(ctx, &s->promise);
    v->val(ctx, &s->func);
    v->val(ctx, &s->value);
    v->val(ctx, &s->stream);
    v->val(ctx, &s->reader);
    for (k = 0; k < 3; k++)
        v->val(ctx, &s->cb[k]);
}

static JSValue js_byte_reader_fini(JSContext *ctx, void *st, bool take_result)
{
    JSByteReaderState *s = st;
    JSValue r = take_result ? s->promise : JS_UNDEFINED;
    int k;

    if (take_result) s->promise = JS_UNDEFINED;
    JS_FreeValue(ctx, s->promise);
    JS_FreeValue(ctx, s->func);
    JS_FreeValue(ctx, s->value);
    JS_FreeValue(ctx, s->stream);
    JS_FreeValue(ctx, s->reader);
    s->promise = s->func = s->value = s->stream = s->reader = JS_UNDEFINED;
    for (k = 0; k < 3; k++) {
        JS_FreeValue(ctx, s->cb[k]);
        s->cb[k] = JS_UNDEFINED;
    }
    return r;
}

/* Stage 0 turns the bytes into the value this reader answers with; stage 1 settles the promise with it, which is
   the request. Nothing in stage 0 runs the page's code — the bytes are the host's and the latch, where there is
   one, is the declaring component's — which is why every reader is one stage and not one machine each. */
static int js_byte_reader_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSByteReaderState *s = st;
    JSValue settled, funcs[2];
    int reject = 0, r;

    if (s->stage == BR_TAKE) {
        const char *bytes = NULL;
        size_t len = 0;
        const ByteReaderIface *f = iface_of(s->hdr.this_val);

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->promise = s->func = s->value = s->stream = s->reader = JS_UNDEFINED;
        if (!f) {
            /* THE RECEIVER CHECK, and it belongs to the reader rather than to the interface: every one of them
               is `Response.prototype.text.call({})`, and there is one answer. */
            JS_ThrowTypeError(ctx, "a byte reader was called on an object of no interface that has one");
            return JS_STEP_ABRUPT;
        }
        DCHECK(s->hdr.arg >= 0 && s->hdr.arg < f->nreaders,
               "a byte reader ran with an index its interface does not declare");
        if (f->take(ctx, s->hdr.this_val, &bytes, &len, &s->stream) < 0) {
            s->value = JS_GetException(ctx);
            reject = 1;
        } else if (!JS_IsUndefined(s->stream)) {
            s->stage = BR_ACQUIRE;
        } else {
            s->value = f->readers[s->hdr.arg].make(ctx, s->hdr.this_val, bytes ? bytes : "", bytes ? len : 0);
            if (JS_IsException(s->value)) { s->value = JS_GetException(ctx); reject = 1; }
        }
        if (s->stage == BR_TAKE) {
            /* The NATIVE capability: %Promise% with no subclass in sight, so building it constructs nothing of
               the page's. Only the settle below is the page's, and that is the request. */
            s->promise = JS_NewPromiseCapability(ctx, funcs);
            if (JS_IsException(s->promise)) return JS_STEP_ABRUPT;
            s->func = funcs[reject];
            JS_FreeValue(ctx, funcs[reject ^ 1]);
            s->stage = BR_SETTLE;
        }
    }

    if (s->stage == BR_ACQUIRE) {
        JSValue op = readable_stream_op(ctx, RS_OP_GET_READER);
        r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), op, s->stream, 0, NULL,
                          cb_result, &s->reader, out_cb, out_argc);
        JS_FreeValue(ctx, op);
        if (r > 0) return r;
        if (JS_IsException(s->reader)) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        s->stage = BR_FIRST_READ;
    }
    if (s->stage == BR_FIRST_READ) {
        const ByteReaderIface *f = iface_of(s->hdr.this_val);
        JSValue read_promise;
        DCHECK(f != NULL, "a byte reader lost its interface between two of its own stages");
        JSValue op = readable_stream_op(ctx, RS_OP_READ);
        r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), op, s->reader, 0, NULL,
                          cb_result, &read_promise, out_cb, out_argc);
        JS_FreeValue(ctx, op);
        if (r > 0) return r;
        if (JS_IsException(read_promise)) return JS_STEP_ABRUPT;
        /* The DRAIN owns the settle from here: it holds the accumulating bytes, it reacts to each read, and it
           resolves with what this reader's `make` builds. So this machine's result IS its promise. */
        s->promise = readable_stream_drain(ctx, s->reader, read_promise, s->hdr.this_val,
                                           f->readers[s->hdr.arg].make);
        JS_FreeValue(ctx, read_promise);
        return JS_IsException(s->promise) ? JS_STEP_ABRUPT : JS_STEP_DONE;
    }

    DCHECK(s->stage == BR_SETTLE, "the byte-read machine was re-entered at a stage it never parks in");
    r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), s->func, JS_UNDEFINED, 1, (JSValueConst *)&s->value,
                      cb_result, &settled, out_cb, out_argc);
    if (r > 0) return r;          /* parked ON THE SETTLE; the `then` read runs with a flow base under it */
    JS_FreeValue(ctx, settled);   /* a resolving function's return value is undefined and unobservable */
    return JS_STEP_DONE;
}

/* ONE machine, one def per reader INDEX — the code is one function and `arg` is the index, which with the
   receiver is the whole of the difference between them.
   WRITTEN OUT, not generated by a macro. engine/check_step_visits.mjs reads these declarations OUT OF THE
   SOURCE to pair each with the visit for its state struct, and a macro is invisible to it exactly as a
   runtime-assembled def is — folding these eight into one macro dropped the gate's host count from 32 to 29
   without changing a line of behaviour, which is the gate going quiet rather than the code getting smaller. */
static const JSTrampStepDef js_byte_reader_defs[BYTE_READER_MAX] = {
    { sizeof(JSByteReaderState), js_byte_reader_step, js_byte_reader_fini, 0, .visit = js_byte_reader_visit },
    { sizeof(JSByteReaderState), js_byte_reader_step, js_byte_reader_fini, 1, .visit = js_byte_reader_visit },
    { sizeof(JSByteReaderState), js_byte_reader_step, js_byte_reader_fini, 2, .visit = js_byte_reader_visit },
    { sizeof(JSByteReaderState), js_byte_reader_step, js_byte_reader_fini, 3, .visit = js_byte_reader_visit },
    { sizeof(JSByteReaderState), js_byte_reader_step, js_byte_reader_fini, 4, .visit = js_byte_reader_visit },
    { sizeof(JSByteReaderState), js_byte_reader_step, js_byte_reader_fini, 5, .visit = js_byte_reader_visit },
    { sizeof(JSByteReaderState), js_byte_reader_step, js_byte_reader_fini, 6, .visit = js_byte_reader_visit },
    { sizeof(JSByteReaderState), js_byte_reader_step, js_byte_reader_fini, 7, .visit = js_byte_reader_visit },
};

int byte_reader_declare(JSContext *ctx, const ByteReaderIface *d)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    int handle = g_iface_n, k;

    DCHECK(g_iface_n < BYTE_READER_IFACE_MAX,
           "more interfaces declared byte readers than this table holds — grow it, the count is fixed because "
           "the platform's is");
    DCHECK(d->nreaders > 0 && d->nreaders <= BYTE_READER_MAX,
           "an interface declared more byte readers than there are step defs for — add defs, they are static "
           "because the gate that checks them reads the source");
    for (k = 0; k < d->nreaders; k++)
        DCHECK(d->readers[k].name && d->readers[k].make, "a byte reader was declared with no name or no maker");
    g_iface[handle] = d;
    for (k = 0; k < d->nreaders; k++) {
        g_iface_stepid[handle][k] = JS_RegisterStepDef(rt, &js_byte_reader_defs[k]);
        CHECK(g_iface_stepid[handle][k] >= 0, "byte reader: no step id — the bytes would be unreadable");
    }
    g_iface_n++;
    return handle;
}

void byte_reader_install(JSContext *ctx, JSValueConst proto, int handle)
{
    const ByteReaderIface *d;
    int k;
    DCHECK(handle >= 0 && handle < g_iface_n, "byte readers were installed with a handle nothing declared");
    d = g_iface[handle];
    for (k = 0; k < d->nreaders; k++)
        JS_SetPropertyStr(ctx, (JSValue)proto, d->readers[k].name,
                          JS_NewCFunction2(ctx, NULL, d->readers[k].name, 0, JS_CFUNC_step,
                                           g_iface_stepid[handle][k]));
}

/* ---- the readers both specs declare in the same words ------------------------------------------------------ */

JSValue byte_reader_text(JSContext *ctx, JSValueConst recv, const char *bytes, size_t len)
{
    /* UTF-8 decode, which is what JS_NewStringLen performs over the host's bytes. */
    (void)recv;
    return JS_NewStringLen(ctx, bytes, len);
}

JSValue byte_reader_json(JSContext *ctx, JSValueConst recv, const char *bytes, size_t len)
{
    /* The REAL parser, so a malformed body rejects with the SyntaxError the page would actually catch rather
       than a placeholder this engine invented. */
    (void)recv;
    return JS_ParseJSON(ctx, bytes, len, "<body>");
}

/* A COPY, for both of these, because what the page gets is ITS OWN to detach, transfer or write through —
   handing out the object's storage would let one flow mutate a reply another is still reading, and a detach
   would leave that flow reading freed memory. */
JSValue byte_reader_array_buffer(JSContext *ctx, JSValueConst recv, const char *bytes, size_t len)
{
    (void)recv;
    return JS_NewArrayBufferCopy(ctx, (const uint8_t *)bytes, len);
}

JSValue byte_reader_bytes(JSContext *ctx, JSValueConst recv, const char *bytes, size_t len)
{
    (void)recv;
    return JS_NewUint8ArrayCopy(ctx, (const uint8_t *)bytes, len);
}
