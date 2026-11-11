/* CompressionStream — the Compression Standard §4 "Interface CompressionStream".
 *
 * The interface includes the Streams Standard's GenericTransformStream mixin: the constructor sets up a
 * TransformStream (Streams §9.3.1) with this file's two algorithms, and `readable`/`writable` answer its halves,
 * as core/encoding/text_stream.c does for Encoding §7.5/§7.6. The codec is core/compression/deflate.h.
 *
 * The compression context (§2 Infrastructure) is the object's record and is written by every chunk, so the
 * record is captured per flow before the codec runs. Unknown input bytes make every chunk the context emits
 * from then on an unknown derived from all input written so far: an RFC 1950/1952 trailer checks every byte, so
 * the context's state depends on the whole history, concrete runs included (their digests name them).
 * Named residual. Not covered: "brotli" (§3 Supported formats, RFC 7932), which the constructor refuses as
 * unsupported. Next diff builds: an RFC 7932 compressor. Absence shows as: `new CompressionStream("brotli")`
 * throwing a TypeError on a page whose feature test expects the format. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/compression/compression_stream.h"
#include "core/compression/deflate.h"
#include "core/encoding/encoding.h"   /* encoding_buffer_source: a brand-tested BufferSource's bytes */
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/streams/transform_stream.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include <lexbor/core/base.h>

/* §4's CompressionFormat, in the IDL's order; CF_BROTLI is listed and unsupported (the file comment). */
enum { CF_BROTLI = 0, CF_DEFLATE, CF_DEFLATE_RAW, CF_GZIP, CF_N };
IDL_ENUM_VALUES(COMPRESSION_FORMATS, "brotli", "deflate", "deflate-raw", "gzip");

/* The object's record: the mixin's transform, §4's format and context, and the identity of the input the
   context has consumed. `held` is the operand list (concolic_buffer_operands' spelling) of every chunk written,
   or JS_UNDEFINED before the first; `held_unknown` says some run in it is unknown, and `held_example` that every
   unknown run's bytes were its example. */
typedef struct {
    JSValue transform;
    JSValue held;
    uint8_t format;
    uint8_t held_unknown, held_example;
    Deflate z;
} CompressionStreamData;
static const uint16_t CS_VALS[] = { offsetof(CompressionStreamData, transform), offsetof(CompressionStreamData, held) };
static const CowRecord CS_REC = { sizeof(CompressionStreamData), CS_VALS, (int)(sizeof CS_VALS / sizeof CS_VALS[0]) };

static JSClassID g_cs_class;
static JSRuntime *g_cs_rt;
static int g_cs_ctor_stepid = -1;
enum { ALG_COMPRESS = 0, ALG_FLUSH, ALG_N };
static int g_alg_stepid[ALG_N];

static void cs_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    CompressionStreamData *d = JS_GetAnyOpaque(val, &id);

    (void)id;
    if (!d) return;
    JS_FreeValueRT(rt, d->transform);
    JS_FreeValueRT(rt, d->held);
    free(d);
}

/* The object holds its TransformStream, whose controller holds the algorithm closures, which hold the object. */
static void cs_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    CompressionStreamData *d = JS_GetAnyOpaque(val, &id);

    (void)id;
    if (!d) return;
    JS_MarkValue(rt, d->transform, mark_func);
    JS_MarkValue(rt, d->held, mark_func);
}

/* Streams §9.3.2's GenericTransformStream mixin. */
enum { GTS_READABLE = 0, GTS_WRITABLE };

static JSValue js_cs_gts_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    CompressionStreamData *d = JS_GetOpaque(this_val, g_cs_class);

    if (!d) return JS_ThrowTypeError(ctx, "not a CompressionStream");
    return JS_DupValue(ctx, magic == GTS_READABLE ? transform_stream_readable(d->transform)
                                                  : transform_stream_writable(d->transform));
}

static DeflateWrapping cs_wrapping(uint8_t format)
{
    DCHECK(format == CF_DEFLATE || format == CF_DEFLATE_RAW || format == CF_GZIP,
           "a CompressionStream holds a format its constructor refuses as unsupported");
    return format == CF_DEFLATE ? DEFLATE_ZLIB : format == CF_DEFLATE_RAW ? DEFLATE_RAW : DEFLATE_GZIP;
}

/* The derivation's operation name: the algorithm and the format, so a chunk and a flush over one history, or
   two formats over one input, are two identities. */
static const char *const CS_OP[ALG_N][CF_N] = {
    { NULL, "CompressionStream(deflate) compress", "CompressionStream(deflate-raw) compress",
      "CompressionStream(gzip) compress" },
    { NULL, "CompressionStream(deflate) flush", "CompressionStream(deflate-raw) flush",
      "CompressionStream(gzip) flush" },
};

/* What a compressed chunk's bytes are of its unknown: the bytes the string's code points are. */
static const char CS_IMAGE[] = "the byte sequence whose isomorphic decode is this compressed chunk";

/* Append `more` (an operand list, consumed) to the record's history. */
static void cs_hold(JSContext *ctx, CompressionStreamData *d, JSValue more)
{
    int64_t n = 0, m = 0, i;

    if (JS_IsUndefined(d->held)) {
        d->held = more;
        return;
    }
    CHECK(JS_GetLength(ctx, d->held, &n) == 0 && JS_GetLength(ctx, more, &m) == 0,
          "compression: an operand list's length could not be read");
    for (i = 0; i < m; i++)
        CHECK(JS_SetPropertyInt64(ctx, d->held, n + i, JS_GetPropertyInt64(ctx, more, i)) >= 0,
              "compression: OOM extending a CompressionStream's input history");
    JS_FreeValue(ctx, more);
}

/* §4's compress and enqueue a chunk steps 1-2, or compress flush and enqueue step 1: the buffer the context
   emits, as a Uint8Array, or JS_UNDEFINED when it is empty (steps 3 / 2: "If buffer is empty, return"), or
   JS_EXCEPTION with step 1's TypeError live. */
static JSValue cs_compress(JSContext *ctx, JSValueConst self, JSValueConst chunk, int which)
{
    CompressionStreamData *d = JS_GetOpaque(self, g_cs_class);
    DeflateOut o = { 0 };
    JSValue out, unknown, example;

    DCHECK(d != NULL, "a CompressionStream algorithm ran over something that is not a CompressionStream");
    cow_capture_host_record(self, d, &CS_REC);
    if (which == ALG_COMPRESS) {
        const uint8_t *p = NULL;
        size_t len = 0;
        JSValue buf = JS_UNDEFINED;
        JSUnknownPiece *u = NULL;
        bool ex = true;
        int nu;

        if (concolic_is(chunk))
            DFAIL("§4 compress and enqueue a chunk step 1 was handed a chunk that is unknown input as a whole — "
                  "build step 1's BufferSource test as a fork over the unknown's type");
        /* Step 1: "If chunk is not a BufferSource type, then throw a TypeError." The chunk is what the page
           wrote, which no declaration brand-tested. */
        if (!JS_IsArrayBuffer(chunk) && JS_GetTypedArrayType(chunk) < 0 && !JS_IsDataView(chunk))
            return JS_ThrowTypeError(ctx, "a CompressionStream chunk is not a BufferSource");
        if (encoding_buffer_source(ctx, chunk, &p, &len, &buf) < 0)
            return JS_EXCEPTION;
        nu = JS_BufferSourceUnknowns(ctx, chunk, &u);
        JS_FreeBufferUnknowns(ctx, u, nu);
        if (len || nu) {
            cs_hold(ctx, d, concolic_buffer_operands(ctx, chunk, p, len, true, &ex));
            d->held_unknown = d->held_unknown || nu > 0;
            d->held_example = d->held_example && ex;
        }
        deflate_write(&d->z, p, len, &o);
        JS_FreeValue(ctx, buf);
    } else {
        deflate_finish(&d->z, &o);
    }
    if (o.n == 0) {
        free(o.b);
        return JS_UNDEFINED;
    }
    if (!d->held_unknown) {
        out = JS_NewUint8ArrayCopy(ctx, o.b, o.n);
        free(o.b);
        return out;
    }
    /* The chunk is an unknown derived from the whole history, whose example is the real compressor's output
       over the runs' examples as Infra's isomorphic decode spells bytes as a string (absent where some run had
       none); the Uint8Array is that string's image, so each element derives from it. */
    if (d->held_example) {
        uint16_t *w = lexbor_malloc(o.n * sizeof *w);
        size_t i;

        CHECK(w != NULL, "compression: OOM spelling a compressed chunk's example");
        for (i = 0; i < o.n; i++) w[i] = o.b[i];
        example = JS_NewStringUTF16(ctx, w, o.n);
        lexbor_free(w);
        CHECK(!JS_IsException(example), "compression: OOM spelling a compressed chunk's example");
    } else {
        example = JS_UNDEFINED;
    }
    unknown = concolic_new_derived_list(ctx, CS_OP[which][d->format], d->held, example);
    DCHECK(!JS_IsUninitialized(unknown),
           "a CompressionStream recorded unknown input in its history and the derivation found none");
    out = d->held_example ? JS_NewUint8ArrayImage(ctx, o.b, o.n, unknown, CS_IMAGE)
                          : JS_NewUint8ArrayImage(ctx, NULL, 0, unknown, CS_IMAGE);
    JS_FreeValue(ctx, unknown);
    free(o.b);
    return out;
}

/* Where the two algorithms rest: the codec, then §9.3.1's enqueue, which is §6.4's controller operation. */
#define CA_STAGES(X) \
    X(CA_ENTRY, "Compression §4 compress and enqueue a chunk / compress flush and enqueue (which algorithm, and " \
                "the stream it belongs to)") \
    X(CA_BUILD, "Compression §4 compress and enqueue a chunk steps 1-4 / compress flush and enqueue steps 1-3 " \
                "(the buffer the context emits, as one Uint8Array)") \
    X(CA_ENQUEUE, "Compression §4 compress and enqueue a chunk step 5 / compress flush and enqueue step 4 " \
                  "(enqueue array in cs's transform)") \
    X(CA_DONE, "Compression §4 (the algorithm is complete; an empty buffer is enqueued nowhere)")
enum { CA_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CA_STEPS[] = { CA_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;
    uint8_t phase;
    JSValue self;      /* the CompressionStream (owned) */
    JSValue chunk;     /* the Uint8Array to enqueue (owned) */
    JSValue cb[3];     /* step_call_run's buffer: [this, enqueue, chunk] */
} JSCaState;

static void js_ca_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSCaState *s = st;
    int k;

    v->val(ctx, &s->self);
    v->val(ctx, &s->chunk);
    for (k = 0; k < 3; k++) v->val(ctx, &s->cb[k]);
}

static int js_ca_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSCaState *s = st;
    JSStepHdr *hdr = &s->hdr;
    int which = hdr->arg;
    JSValue out;
    int r;

    /* §6 calls a transform algorithm with « chunk, controller » and a flush algorithm with « controller ». */
    DCHECK(which == ALG_COMPRESS || which == ALG_FLUSH,
           "a CompressionStream algorithm ran with an operation this component does not have");
    if (hdr->stage == CA_ENTRY) {
        s->self = JS_DupValue(ctx, JS_StepClosureData(hdr, 0));
        s->chunk = JS_UNDEFINED;
        s->cb[0] = s->cb[1] = s->cb[2] = JS_UNDEFINED;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        STEP_GOTO(hdr->stage, CA_BUILD, &s->phase, NULL);
    }
    if (hdr->stage == CA_BUILD) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->chunk = cs_compress(ctx, s->self, which == ALG_COMPRESS ? step_arg(hdr, 0) : JS_UNDEFINED, which);
        if (JS_IsException(s->chunk)) { s->chunk = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        if (JS_IsUndefined(s->chunk)) {
            STEP_GOTO(hdr->stage, CA_DONE, &s->phase, NULL);
            return JS_STEP_DONE;
        }
        STEP_GOTO(hdr->stage, CA_ENQUEUE, &s->phase, NULL);
    }
    if (hdr->stage == CA_ENQUEUE) {
        JSValueConst arg = s->chunk;
        JSValue op = transform_stream_op(ctx, TS_OP_ENQUEUE);

        r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), op, step_arg(hdr, which == ALG_COMPRESS ? 1 : 0),
                          1, &arg, cb_result, &out, out_cb, out_argc);
        JS_FreeValue(ctx, op);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, out);
        STEP_GOTO(hdr->stage, CA_DONE, &s->phase, NULL);
        return JS_STEP_DONE;
    }
    DCHECK(hdr->stage == CA_DONE, "a CompressionStream algorithm resumed at a stage §4 does not have");
    JS_FreeValue(ctx, cb_result);
    return JS_STEP_DONE;
}

#define CA_DEF(i) { sizeof(JSCaState), js_ca_step, NULL, (i), .visit = js_ca_visit, \
                   .algorithm = "Compression §4 compress and enqueue a chunk / compress flush and enqueue", \
                   .steps = CA_STEPS }
static const JSTrampStepDef js_ca_defs[ALG_N] = { CA_DEF(ALG_COMPRESS), CA_DEF(ALG_FLUSH) };
#undef CA_DEF

/* The constructor: steps 1-4 build the object and its two algorithms; steps 5-6 are §9.3.1's set up, a call of
   §6's operation and the one place it suspends. */
#define CC_STAGES(X) \
    X(CC_ENTRY = IDL_STEP_FIRST, "Compression §4 new CompressionStream(format) steps 1-4 (the format, and the " \
                                 "transform and flush algorithms)") \
    X(CC_SETUP, "Compression §4 new CompressionStream(format) steps 5-6 (a new TransformStream, set up with " \
                "those algorithms)") \
    X(CC_DONE, "Compression §4 new CompressionStream(format) (this's transform is the TransformStream)")
enum { CC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CC_STEPS[] = { CC_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t phase;
    JSValue obj;       /* the CompressionStream being built (owned) */
    JSValue fns[3];    /* transformAlgorithm, flushAlgorithm, cancelAlgorithm (owned) */
    JSValue cb[5];     /* step_call_run's buffer: [this, setup, 3 algorithms] */
} JSCcState;

static void js_cc_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSCcState *s = st;
    int k;

    v->val(ctx, &s->obj);
    for (k = 0; k < 3; k++) v->val(ctx, &s->fns[k]);
    for (k = 0; k < 5; k++) v->val(ctx, &s->cb[k]);
}

static int js_cc_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                      JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSCcState *s = st;
    JSValue out;
    int r, k;

    if (hdr->stage == CC_ENTRY) {
        CompressionStreamData *d;
        const char *f;
        int format = -1;
        JSValueConst self;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->obj = JS_UNDEFINED;
        s->fns[0] = s->fns[1] = s->fns[2] = JS_UNDEFINED;
        for (k = 0; k < 5; k++) s->cb[k] = JS_UNDEFINED;
        if (JS_IsUndefined(hdr->this_val))
            return JS_ThrowTypeError(ctx, "constructor CompressionStream requires 'new'"), -1;
        DCHECK(argc == 1, "§4's constructor declares one required position, so the machine hands this body one");
        /* Web IDL §3.2.18 has already refused a string §4's enum does not list, so this names one of four. */
        f = JS_ToCString(ctx, argv[0]);
        if (!f) return -1;
        for (k = 0; k < CF_N; k++)
            if (!strcmp(f, COMPRESSION_FORMATS[k])) format = k;
        JS_FreeCString(ctx, f);
        DCHECK(format >= 0, "a CompressionFormat crossed Web IDL §3.2.18's conversion and names no listed value");
        /* Step 1: "If format is unsupported in CompressionStream, then throw a TypeError." */
        if (format == CF_BROTLI)
            return JS_ThrowTypeError(ctx, "CompressionStream: the \"brotli\" format is not supported"), -1;
        {
            JSValue proto = JS_GetClassProto(ctx, g_cs_class);
            DCHECK(!JS_IsNull(proto), "a CompressionStream was minted in a realm that never ran its install");
            s->obj = JS_NewObjectProtoClass(ctx, proto, g_cs_class);
            JS_FreeValue(ctx, proto);
        }
        if (JS_IsException(s->obj)) { s->obj = JS_UNDEFINED; return -1; }
        /* The record is complete before it is attached, so a finalizer after a failure below frees only it. */
        d = lexbor_calloc(1, sizeof *d);
        CHECK(d, "compression: OOM building a CompressionStream");
        d->transform = d->held = JS_UNDEFINED;
        d->format = (uint8_t)format;   /* step 2 */
        d->held_example = 1;
        deflate_init(&d->z, cs_wrapping(d->format));
        JS_SetOpaque(s->obj, d);
        self = s->obj;
        /* Steps 3-4. §6 calls the transform algorithm « chunk, controller » and the flush « controller ». */
        for (k = 0; k < ALG_N; k++) {
            s->fns[k] = JS_NewStepClosure(ctx, g_alg_stepid[k], 2, 1, &self);
            if (JS_IsException(s->fns[k])) { s->fns[k] = JS_UNDEFINED; return -1; }
        }
        hdr->stage = CC_SETUP;
    }
    DCHECK(hdr->stage == CC_SETUP, "a CompressionStream constructor resumed at a stage §4 does not have");
    {
        JSValue op = transform_stream_op(ctx, TS_OP_SETUP);
        r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), op, JS_UNDEFINED, 3, (JSValueConst *)s->fns,
                          cb_result, &out, out_cb, out_argc);
        JS_FreeValue(ctx, op);
    }
    if (r > 0) return r;
    if (JS_IsException(out)) return -1;
    {
        CompressionStreamData *d = JS_GetOpaque(s->obj, g_cs_class);
        DCHECK(d != NULL, "a CompressionStream constructor lost its own object between two stages");
        JS_FreeValue(ctx, d->transform);
        d->transform = out;   /* step 5's "Set this's transform" */
    }
    STEP_GOTO(hdr->stage, CC_DONE, &s->phase, NULL);
    *presult = s->obj;
    s->obj = JS_UNDEFINED;
    return 0;
}

static const IdlStepDecl js_cc_decl = {
    js_cc_step, sizeof(JSCcState), js_cc_visit, NULL, "Compression §4 new CompressionStream(format)", CC_STEPS
};

void compression_stream_init(JSContext *ctx)
{
    JSClassDef def = { "CompressionStream", .finalizer = cs_finalizer, .gc_mark = cs_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType CTOR_ARGS[1] = { IDL_ENUM };
    int i;

    DCHECK(g_cs_rt == NULL || g_cs_rt == rt, "CompressionStream was installed into a second runtime");
    if (g_cs_rt == rt) return;
    g_cs_rt = rt;
    JS_NewClassID(rt, &g_cs_class);
    JS_NewClass(rt, g_cs_class, &def);
    agent_state_class("compression_stream", &g_cs_class,
                      "Compression §4's CompressionStream class — the brand the mixin's getters and the two "
                      "algorithms read, and the per-realm prototype slot");
    for (i = 0; i < ALG_N; i++) {
        g_alg_stepid[i] = JS_RegisterStepDef(rt, &js_ca_defs[i]);
        CHECK(g_alg_stepid[i] >= 0, "compression: no step id for a CompressionStream algorithm");
    }
    g_cs_ctor_stepid = idl_method_id_step(ctx, CTOR_ARGS, 1, NULL, 0, &js_cc_decl, 0);
    idl_arg_enum(0, COMPRESSION_FORMATS);
    realm_declare_intrinsic(compression_stream_install_realm);
}

void compression_stream_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global, ctor;

    DCHECK(g_cs_ctor_stepid >= 0, "a realm asked for CompressionStream before compression_stream_init declared it");
    prev = JS_GetClassProto(ctx, g_cs_class);
    DCHECK(JS_IsNull(prev), "compression_stream_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    global = JS_GetGlobalObject(ctx);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "CompressionStream.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "CompressionStream");
    idl_install_accessor(ctx, proto, "readable", js_cs_gts_get, GTS_READABLE, -1);
    idl_install_accessor(ctx, proto, "writable", js_cs_gts_get, GTS_WRITABLE, -1);
    ctor = idl_step_constructor(ctx, "CompressionStream", g_cs_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the CompressionStream interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    idl_define_global_property_reference(ctx, global, "CompressionStream", ctor);
    JS_SetClassProto(ctx, g_cs_class, proto);   /* the realm owns it from here */
    JS_FreeValue(ctx, global);
}

void compression_stream_free(void)
{
    int i;

    if (!g_cs_rt) return;
    g_cs_rt = NULL;
    g_cs_class = 0;   /* core/agent_state.h: a carried id names a class in a runtime that is gone */
    g_cs_ctor_stepid = -1;
    for (i = 0; i < ALG_N; i++) g_alg_stepid[i] = -1;
}
