/* TextDecoderStream AND TextEncoderStream — the Encoding Standard §7.5 and §7.6.
 *
 * WHAT THEY ARE. Neither is a subclass of TransformStream. Each is its own Web IDL interface that INCLUDES the
 * Streams Standard's GenericTransformStream mixin, which means it has an associated `transform` — an actual
 * TransformStream — and its `readable`/`writable` getters answer that stream's two halves. The constructor
 * creates one and SETS IT UP (Streams §9.3.1) with algorithms of this component's, so the transform stream's
 * fixed marks and identity transform never come into it: writes go through this file's codec.
 *
 * THE CODEC IS §7.1's AND §7.2's, NOT A SECOND ONE. §7.5 says "set stream's decoder to a new instance of
 * encoding's decoder" — the same object a TextDecoder holds, keeping its half-read sequence between chunks —
 * so encoding.c exports it and this file drives it. Writing the decode loop again here is how the streaming
 * interface and the non-streaming one come to disagree about a sequence split across a chunk boundary, which
 * is the one thing the streaming interface exists to get right.
 *
 * EVERY ALGORITHM IS A STEP, because every one of them ENQUEUES — and enqueueing is
 * TransformStreamDefaultControllerEnqueue, a §6.4 controller operation, which is the page-reachable machinery
 * and therefore a request. So the four algorithms (decode-and-enqueue, flush-and-enqueue, encode-and-enqueue,
 * encode-and-flush) are step definitions rather than C functions, minted as step CLOSURES over the
 * TextDecoderStream/TextEncoderStream object so each one can reach its own codec state.
 *
 * §7.6's ENCODER TAKES A DOMString, NOT A USVString, AND THAT IS THE WHOLE DESIGN. A surrogate PAIR split
 * across two chunks must come out as the one scalar value it encodes, which is only possible if the lone
 * leading surrogate at the end of a chunk is HELD rather than replaced. So the chunk is read as UTF-16 code
 * units (CESU-8, which is how this engine hands a string's lone surrogates to C) and §7.6's "convert code unit
 * to scalar value" machine decides each one. A USVString conversion would have replaced the split pair with
 * two U+FFFD before this file ever saw it. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/encoding/encoding.h"
#include "core/encoding/text_stream.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/streams/pipe.h"
#include "core/streams/readable_stream.h"
#include "core/streams/transform_stream.h"

/* ---- the two interfaces' state ----------------------------------------------------------------------------- */

/* THE GenericTransformStream MIXIN'S `transform`, plus whichever codec this object is. One record for both
   interfaces because the mixin half is identical and the codec half is a union of two small things — two
   records would duplicate the transform slot, the finalizer and the gc_mark, which is three chances for one of
   them to be updated and the other not. `dec` is NULL exactly when this is an encoder. */
typedef struct {
    JSValue transform;      /* the TransformStream this object's readable/writable come from (owned) */
    EncDecoder *dec;        /* §7.5's "stream's decoder", or NULL for §7.6 */
    /* §7.6's "leading surrogate": the code unit held back so a pair split across chunks can be rejoined.
       -1 when there is none, which is the standard's null. */
    int32_t lead;
} TextStreamData;

static JSClassID g_tds_class;
static JSClassID g_tes_class;

static JSRuntime *g_ts_rt;
static int       g_tds_ctor_stepid = -1, g_tes_ctor_stepid = -1;

/* The four algorithms' step ids, and the two constructors' — see the file comment. */
enum { ALG_DECODE = 0, ALG_DEC_FLUSH, ALG_ENCODE, ALG_ENC_FLUSH, ALG_N };
static int g_alg_stepid[ALG_N];

static TextStreamData *tds_of(JSValueConst v)
{
    TextStreamData *t = JS_GetOpaque(v, g_tds_class);
    return t;
}

static TextStreamData *tes_of(JSValueConst v)
{
    return JS_GetOpaque(v, g_tes_class);
}

/* Either interface — what the mixin's two getters and the algorithms need, and neither cares which. */
static TextStreamData *ts_any_of(JSValueConst v)
{
    TextStreamData *t = JS_GetOpaque(v, g_tds_class);
    return t ? t : (TextStreamData *)JS_GetOpaque(v, g_tes_class);
}

static void text_stream_finalizer(JSRuntime *rt, JSValue val)
{
    TextStreamData *t = JS_GetOpaque(val, g_tds_class);
    if (!t) t = JS_GetOpaque(val, g_tes_class);
    if (!t) return;
    JS_FreeValueRT(rt, t->transform);
    enc_decoder_free(t->dec);
    free(t);
}

/* THE CYCLE IS REAL AND MUST BE MARKED. This object holds its TransformStream; that stream's controller holds
   the algorithm closures; each closure holds this object. Without a mark the collector cannot see the loop and
   every TextDecoderStream a page makes is a leak the runtime's own walk reports at teardown. */
static void text_stream_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    TextStreamData *t = JS_GetOpaque(val, g_tds_class);
    if (!t) t = JS_GetOpaque(val, g_tes_class);
    if (!t) return;
    JS_MarkValue(rt, t->transform, mark_func);
}

/* ---- Streams §9.3.2's GenericTransformStream mixin ---------------------------------------------------------- */

enum { GTS_READABLE = 0, GTS_WRITABLE };

static JSValue js_gts_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    TextStreamData *t = ts_any_of(this_val);
    if (!t) return JS_ThrowTypeError(ctx, "not a TextDecoderStream or TextEncoderStream");
    return JS_DupValue(ctx, magic == GTS_READABLE ? transform_stream_readable(t->transform)
                                                  : transform_stream_writable(t->transform));
}

/* ---- §7.5's TextDecoderCommon attributes -------------------------------------------------------------------- */

enum { TDS_ENCODING = 0, TDS_FATAL, TDS_IGNORE_BOM };

static JSValue js_tds_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    TextStreamData *t = tds_of(this_val);
    if (!t) return JS_ThrowTypeError(ctx, "not a TextDecoderStream");
    DCHECK(t->dec != NULL, "a TextDecoderStream reached its attributes with no decoder — the constructor "
                           "builds one before the object is reachable");
    /* §7.1 Interface mixin TextDecoderCommon: "return this's encoding's name, ASCII lowercased". §7.5's
       TextDecoderStream includes that mixin, so it answers the SAME lowercased half of §4.2's Name column
       TextDecoder does — not DOM §4.5's characterSet, which is the other column. */
    if (magic == TDS_ENCODING)
        return JS_NewString(ctx, encoding_name_ascii_lowercased(enc_decoder_encoding(t->dec)));
    if (magic == TDS_FATAL) return JS_NewBool(ctx, enc_decoder_fatal(t->dec));
    DCHECK(magic == TDS_IGNORE_BOM,
           "a TextDecoderStream attribute was declared with a magic this component does not answer");
    return JS_NewBool(ctx, enc_decoder_ignore_bom(t->dec));
}

static JSValue js_tes_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!tes_of(this_val)) return JS_ThrowTypeError(ctx, "not a TextEncoderStream");
    return JS_NewString(ctx, "utf-8");   /* §7.6: a TextEncoderStream offers no label and is always UTF-8 */
}

/* ---- §7.6's "convert code unit to scalar value" ------------------------------------------------------------- */

/* THE CHUNK AS UTF-16 CODE UNITS. quickjs hands a string to C as UTF-8, which has already replaced a lone
   surrogate — so the CESU-8 form is asked for instead, where each surrogate is its own three-byte sequence and
   §7.6's machine can see it. Returns the unit at `*pi` and advances past it. */
static uint32_t cesu8_next(const uint8_t *p, size_t len, size_t *pi)
{
    size_t i = *pi;
    uint32_t c = p[i];
    int n;

    DCHECK(i < len, "a code unit was read past the end of the chunk");
    if (c < 0x80) { *pi = i + 1; return c; }
    n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
    DCHECK(i + (size_t)n <= len, "a CESU-8 sequence ran past the end of the buffer this engine produced");
    c &= (uint32_t)(0xFF >> (n + 1));
    for (i++; i < *pi + (size_t)n; i++) c = (c << 6) | (p[i] & 0x3F);
    *pi = i;
    /* CESU-8 never produces a four-byte sequence, but a plain-UTF-8 buffer would; a supplementary code point
       is two units here, and answering it whole would lose the split-pair behaviour this whole path is for. */
    DCHECK(n < 4, "a supplementary code point arrived as one unit — the chunk must be read as CESU-8");
    return c;
}

/* Append `cp` to `o` as UTF-8, growing it. §7.6's output is UTF-8 bytes, so this IS the encoder. */
typedef struct { uint8_t *b; size_t n, cap; } ByteBuf;

static void bb_put(ByteBuf *o, uint32_t cp)
{
    if (o->n + 4 > o->cap) {
        size_t c = o->cap ? o->cap * 2 : 64;
        while (o->n + 4 > c) c *= 2;
        o->b = realloc(o->b, c);
        CHECK(o->b != NULL, "encoding: OOM growing a TextEncoderStream chunk");
        o->cap = c;
    }
    if (cp < 0x80) {
        o->b[o->n++] = (uint8_t)cp;
    } else if (cp < 0x800) {
        o->b[o->n++] = (uint8_t)(0xC0 | (cp >> 6));
        o->b[o->n++] = (uint8_t)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o->b[o->n++] = (uint8_t)(0xE0 | (cp >> 12));
        o->b[o->n++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        o->b[o->n++] = (uint8_t)(0x80 | (cp & 0x3F));
    } else {
        o->b[o->n++] = (uint8_t)(0xF0 | (cp >> 18));
        o->b[o->n++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
        o->b[o->n++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        o->b[o->n++] = (uint8_t)(0x80 | (cp & 0x3F));
    }
}

/* ---- the four algorithms ------------------------------------------------------------------------------------
 *
 * ONE MACHINE, FOUR ENTRIES, because all four end the same way: build a chunk out of the codec, and if it is
 * not empty, ENQUEUE it — one request, made from one place. What differs is only how the chunk is built, which
 * is the part that runs no page code and needs no stages. */

/* WHERE THIS MACHINE RESTS. §7.5 and §7.6 give the two streams four algorithms, and the page's code runs at
   exactly two points in them: the ToString §7.6 step 1 performs on what was written, and the enqueue. */
#define TX_STAGES(X) \
    X(TX_ENTRY, "Encoding §7.5/§7.6 (which of the four transform and flush algorithms this entry is, and the " \
                "stream it belongs to)") \
    X(TX_STRING, "Encoding §7.6 encode and enqueue a chunk step 1 (chunk converted to a DOMString — the " \
                 "page's own `toString`)") \
    X(TX_BUILD, "Encoding §7.5/§7.6 (the codec: decode-and-enqueue steps 1-4, decode-and-flush steps 1-2, " \
                "encode-and-enqueue steps 2-5, encode-and-flush steps 1-2)") \
    X(TX_ENQUEUE, "Encoding §7.5/§7.6 (enqueue the chunk in stream — §6.3's " \
                  "TransformStreamDefaultControllerEnqueue over the controller §6 handed the algorithm)") \
    X(TX_DONE, "Encoding §7.5/§7.6 (the algorithm is complete; an empty chunk is enqueued nowhere)")
enum { TX_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TX_STEPS[] = { TX_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;
    uint8_t phase;
    JSValue self;      /* the TextDecoderStream / TextEncoderStream (owned) */
    JSValue input;     /* §7.6 step 1's DOMString, once ToString has run on what was written (owned) */
    JSValue chunk;     /* what this algorithm produced, until it has been enqueued (owned) */
    JSValue cb[3];     /* step_call_run's buffer: [this, enqueue, chunk] */
} JSTxState;

static void js_tx_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSTxState *s = st;
    int k;
    v->val(ctx, &s->self);
    v->val(ctx, &s->input);
    v->val(ctx, &s->chunk);
    for (k = 0; k < 3; k++) v->val(ctx, &s->cb[k]);
}

/* DELETED: js_tx_release, a second copy of js_tx_visit's list. §9.3.1's wrapper discards a non-promise result
   and all four algorithms answer undefined, so this machine has no completion to state at all. */

/* §7.5's "decode and enqueue a chunk" and "flush and enqueue" — the same decoder, differing only in whether
   this is the end of the stream. Returns the produced string, or JS_EXCEPTION with the TypeError live. */
static JSValue tx_decode(JSContext *ctx, TextStreamData *t, JSValueConst chunk, bool flush)
{
    const uint8_t *p = NULL;
    size_t len = 0;
    JSValue buf = JS_UNDEFINED, r;

    DCHECK(t->dec != NULL, "a decode algorithm ran on a TextEncoderStream");
    if (!flush) {
        /* §7.5 step 1: "convert chunk to an AllowSharedBufferSource". The chunk is whatever the page WROTE, so
           nothing has brand-tested it — a transform algorithm reached through set up receives the raw value,
           unlike an IDL member whose declaration converts first. Anything that is not a buffer source is the
           union's TypeError, which errors both halves of the stream, which is what the standard wants. */
        if (!JS_IsArrayBuffer(chunk) && JS_GetTypedArrayType(chunk) < 0 && !JS_IsDataView(chunk))
            return JS_ThrowTypeError(ctx, "a TextDecoderStream chunk is not a BufferSource");
        if (encoding_buffer_source(ctx, chunk, &p, &len, &buf) < 0)
            return JS_EXCEPTION;
    }
    r = enc_decoder_decode(ctx, t->dec, p, len, !flush);
    JS_FreeValue(ctx, buf);
    return r;
}

/* §7.6's "encode and enqueue a chunk", from step 2 on: `input` is already the DOMString step 1 produced, which
   the machine below obtained as a REQUEST because ToString runs the page's own `toString`. */
static JSValue tx_encode(JSContext *ctx, TextStreamData *t, JSValueConst chunk)
{
    const char *s;
    size_t len = 0, i = 0;
    ByteBuf o = { 0 };
    JSValue r;

    DCHECK(t->dec == NULL, "an encode algorithm ran on a TextDecoderStream");
    s = JS_ToCStringLen2(ctx, &len, chunk, true);   /* CESU-8: lone surrogates survive as their own units */
    if (!s) return JS_EXCEPTION;
    while (i < len) {
        uint32_t u = cesu8_next((const uint8_t *)s, len, &i);
        /* §7.6's convert code unit to scalar value, in the standard's own order. */
        if (t->lead >= 0) {
            uint32_t leading = (uint32_t)t->lead;
            t->lead = -1;
            if (u >= 0xDC00 && u <= 0xDFFF) {
                bb_put(&o, 0x10000 + ((leading - 0xD800) << 10) + (u - 0xDC00));
                continue;
            }
            /* "Restore item to input" — the unit is not a trailing surrogate, so the HELD one is the error and
               this one is read again on the next turn of the loop. */
            bb_put(&o, 0xFFFD);
            /* fall through to classify `u` itself */
        }
        if (u >= 0xD800 && u <= 0xDBFF) { t->lead = (int32_t)u; continue; }
        if (u >= 0xDC00 && u <= 0xDFFF) { bb_put(&o, 0xFFFD); continue; }
        bb_put(&o, u);
    }
    JS_FreeCString(ctx, s);
    r = o.n ? JS_NewUint8ArrayCopy(ctx, o.b, o.n) : JS_UNDEFINED;
    free(o.b);
    return r;
}

/* §7.6's "encode and flush": a leading surrogate left over at the end of the stream is U+FFFD and nothing
   else, because there is no chunk left for its pair to arrive in. */
static JSValue tx_encode_flush(JSContext *ctx, TextStreamData *t)
{
    static const uint8_t REPLACEMENT[3] = { 0xEF, 0xBF, 0xBD };
    DCHECK(t->dec == NULL, "an encode flush ran on a TextDecoderStream");
    if (t->lead < 0) return JS_UNDEFINED;
    t->lead = -1;
    return JS_NewUint8ArrayCopy(ctx, REPLACEMENT, sizeof REPLACEMENT);
}

/* IS THERE ANYTHING TO ENQUEUE? Both standards say "if outputChunk is not the empty string" / "if output is not
   empty" — an algorithm that produced nothing enqueues nothing, so a chunk that only advanced the decoder's
   state does not put an empty string on the readable side. The encoders answer undefined for "nothing"; the
   decoder answers a string, and this is the one place its emptiness is asked. The comparison runs no page code:
   the value is a primitive string this file just made. */
static bool tx_has_chunk(JSContext *ctx, JSValueConst v)
{
    if (JS_IsUndefined(v)) return false;
    if (JS_IsString(v)) {
        size_t len = 0;
        const char *p = JS_ToCStringLen(ctx, &len, v);
        bool any;
        DCHECK(p != NULL, "a decoded chunk this component built could not be read back as bytes");
        any = len != 0;
        JS_FreeCString(ctx, p);
        return any;
    }
    return true;
}

static int js_tx_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSTxState *s = st;
    JSStepHdr *hdr = &s->hdr;
    TextStreamData *t;
    JSValue out;
    int r;

    /* WHICH ALGORITHM, and where §6 put the controller. A TRANSFORM algorithm is called « chunk, controller »
       and a FLUSH algorithm « controller » — so the controller is argument 1 for the two that take a chunk and
       argument 0 for the two that do not. Reading it from the wrong slot hands `enqueue` an undefined receiver
       and every flush fails its brand test. */
    int which = hdr->arg;
    bool takes_chunk = (which == ALG_DECODE || which == ALG_ENCODE);

    DCHECK(which >= 0 && which < ALG_N,
           "an Encoding stream algorithm ran with an operation this component does not have");

    if (s->hdr.stage == TX_ENTRY) {
        s->self = JS_DupValue(ctx, JS_StepClosureData(hdr, 0));
        s->input = s->chunk = JS_UNDEFINED;
        s->cb[0] = s->cb[1] = s->cb[2] = JS_UNDEFINED;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* §7.6 step 1 is ToString on what the page wrote, which is the page's own `toString`. §7.5's chunk is
           a buffer source and needs no coercion, so only the encoder takes this stage. */
        STEP_GOTO(s->hdr.stage, which == ALG_ENCODE ? TX_STRING : TX_BUILD, &s->phase, &s->hdr.get_phase, NULL);
    }

    if (s->hdr.stage == TX_STRING) {
        r = step_tostring_run(ctx, hdr, step_arg(hdr, 0), cb_result, &s->input, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        STEP_GOTO(s->hdr.stage, TX_BUILD, &s->phase, &s->hdr.get_phase, NULL);
    }

    if (s->hdr.stage == TX_BUILD) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        t = ts_any_of(s->self);
        DCHECK(t != NULL, "an Encoding stream algorithm ran over something that is not one of its interfaces");
        s->chunk = which == ALG_DECODE    ? tx_decode(ctx, t, step_arg(hdr, 0), false)
                 : which == ALG_DEC_FLUSH ? tx_decode(ctx, t, JS_UNDEFINED, true)
                 : which == ALG_ENCODE    ? tx_encode(ctx, t, s->input)
                                          : tx_encode_flush(ctx, t);
        if (JS_IsException(s->chunk)) { s->chunk = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        if (!tx_has_chunk(ctx, s->chunk)) {
            STEP_GOTO(s->hdr.stage, TX_DONE, &s->phase, &s->hdr.get_phase, NULL);
            return JS_STEP_DONE;
        }
        STEP_GOTO(s->hdr.stage, TX_ENQUEUE, &s->phase, &s->hdr.get_phase, NULL);
    }

    if (s->hdr.stage == TX_ENQUEUE) {
        /* §9.3.1's "enqueue chunk in stream" IS TransformStreamDefaultControllerEnqueue, reached with the
           controller §6 handed this algorithm as `this` — never through a property the page could replace. */
        JSValueConst arg = s->chunk;
        JSValue op = transform_stream_op(ctx, TS_OP_ENQUEUE);
        r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), op,
                          step_arg(hdr, takes_chunk ? 1 : 0), 1, &arg, cb_result, &out, out_cb, out_argc);
        JS_FreeValue(ctx, op);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, out);
        STEP_GOTO(s->hdr.stage, TX_DONE, &s->phase, &s->hdr.get_phase, NULL);
        return JS_STEP_DONE;
    }

    DCHECK(s->hdr.stage == TX_DONE,
           "an Encoding stream algorithm resumed at a stage §7.5 and §7.6 do not have between them");
    JS_FreeValue(ctx, cb_result);
    return JS_STEP_DONE;
}

#define TX_DEF(i) { sizeof(JSTxState), js_tx_step, NULL, (i), .visit = js_tx_visit, \
                   .algorithm = "Encoding §7.5/§7.6 the text stream transform and flush algorithms", \
                   .steps = TX_STEPS }
static const JSTrampStepDef js_tx_defs[ALG_N] = { TX_DEF(0), TX_DEF(1), TX_DEF(2), TX_DEF(3) };
#undef TX_DEF

/* ---- the two constructors ------------------------------------------------------------------------------------
 *
 * Both END in a request — Streams §9.3.1's set up is a step, because building a transform stream settles
 * promises — so both are step constructors. What they do before that is their own standard's steps. */

/* WHERE THESE TWO MACHINES REST. §7.5's constructor is four steps and §7.6's is six, and both end in "set up"
   — §9.3.1's set up a TransformStream, which is a CALL of §6's own operation and therefore the one place
   either can suspend. */
#define TC_STAGES(X) \
    X(TC_ENTRY = IDL_STEP_FIRST, \
      "Encoding §7.5 new TextDecoderStream(label, options) steps 1-3 / §7.6 new TextEncoderStream() steps " \
      "1-3 (the encoding and error mode, or the UTF-8 encoder, and the two algorithms)") \
    X(TC_SETUP, "Encoding §7.5 step 4 / §7.6 steps 4-6 (set up a text decoder/encoder stream — §9.3.1's set " \
                "up a TransformStream over those algorithms)") \
    X(TC_DONE, "Encoding §7.5/§7.6 (this's transform is the TransformStream, and the object is the " \
               "constructor's result)")
enum { TC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TC_STEPS[] = { TC_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t phase;
    JSValue obj;       /* the interface object being built (owned) */
    JSValue fns[3];    /* transformAlgorithm, flushAlgorithm, cancelAlgorithm (owned) */
    JSValue cb[5];     /* step_call_run's buffer: [this, setup, 3 algorithms] */
} JSTcState;

static void js_tc_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSTcState *s = st;
    int k;
    v->val(ctx, &s->obj);
    for (k = 0; k < 3; k++) v->val(ctx, &s->fns[k]);
    for (k = 0; k < 5; k++) v->val(ctx, &s->cb[k]);
}

/* Build the object, its codec and the two algorithm closures over it. Returns -1 with an exception live. */
static int tc_build(JSContext *ctx, JSTcState *s, bool decoder, int enc, bool fatal, bool ignore_bom)
{
    TextStreamData *t;
    JSValueConst self;
    int k;

    {
        JSValue proto = JS_GetClassProto(ctx, decoder ? g_tds_class : g_tes_class);
        DCHECK(!JS_IsNull(proto), "a text stream was minted in a realm that never ran its install");
        s->obj = JS_NewObjectProtoClass(ctx, proto, decoder ? g_tds_class : g_tes_class);
        JS_FreeValue(ctx, proto);
    }
    if (JS_IsException(s->obj)) return -1;
    /* THE RECORD IS COMPLETE BEFORE IT IS ATTACHED, so the finalizer of an object torn down by a failure below
       frees exactly what it holds and nothing it never got. */
    t = calloc(1, sizeof *t);
    CHECK(t, "encoding: OOM building a text stream");
    t->transform = JS_UNDEFINED;
    t->dec = decoder ? enc_decoder_new(enc, fatal, ignore_bom) : NULL;
    t->lead = -1;
    JS_SetOpaque(s->obj, t);

    self = s->obj;
    for (k = 0; k < 2; k++) {
        int id = g_alg_stepid[decoder ? (k ? ALG_DEC_FLUSH : ALG_DECODE)
                                      : (k ? ALG_ENC_FLUSH : ALG_ENCODE)];
        /* §6 calls a transform algorithm with « chunk, controller » and a flush algorithm with « controller »;
           declaring 2 and 1 is what makes `step_arg(hdr, 1)` the controller in the first case and
           `step_arg(hdr, 0)` it in the second — which is why the enqueue stage reads argument 1 for both, and
           why the flush definitions are entered with an unused slot rather than a different shape. */
        s->fns[k] = JS_NewStepClosure(ctx, id, 2, 1, &self);
        if (JS_IsException(s->fns[k])) return -1;
    }
    s->fns[2] = JS_UNDEFINED;   /* §7.5 and §7.6 give no cancel algorithm */
    return 0;
}

/* `next` IS THE STAGE THE CALLER CONTINUES AT, and it is a parameter because the set-up is not always the last
   thing its caller does. §7.5's and §7.6's constructors are FINISHED once the transform exists, so they pass
   their own TC_DONE; the decode operation below has a pipe to perform afterwards and passes its own next stage.
   Stated as an argument rather than branched on inside, so this function still has exactly one exit and the
   caller's stage list stays the caller's. */
static int tc_run(JSContext *ctx, JSStepHdr *hdr, JSTcState *s, JSValue cb_result, JSValue *presult,
                  int next, JSValue **out_cb, int *out_argc)
{
    JSValue out;
    int r;

    DCHECK(hdr->stage == TC_SETUP,
           "a text stream constructor resumed at a stage §7.5 and §7.6 do not have between them");
    {
        /* Streams §9.3.1's "set up a TransformStream", reached as a request because building the two halves
           settles promises and resolves capabilities, which is the page's machinery. */
        {
            JSValue op = transform_stream_op(ctx, TS_OP_SETUP);
            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), op, JS_UNDEFINED,
                              3, (JSValueConst *)s->fns, cb_result, &out, out_cb, out_argc);
            JS_FreeValue(ctx, op);
        }
        if (r > 0) return r;
        if (JS_IsException(out)) return -1;
        {
            TextStreamData *t = ts_any_of(s->obj);
            DCHECK(t != NULL, "a text stream constructor lost its own object between two stages");
            JS_FreeValue(ctx, t->transform);
            t->transform = out;
        }
        STEP_GOTO(hdr->stage, next, &s->phase, NULL);
        *presult = s->obj;
        s->obj = JS_UNDEFINED;
        return 0;
    }
}

/* §7.5's constructor: `(optional DOMString label = "utf-8", optional TextDecoderOptions options = {})`. */
static int js_tds_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSTcState *s = st;

    if (hdr->stage == TC_ENTRY) {
        JSValueConst options = argc > 1 ? argv[1] : JS_UNDEFINED;
        const char *label = "utf-8";
        size_t label_len = 5;
        int id;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->obj = JS_UNDEFINED;
        s->fns[0] = s->fns[1] = s->fns[2] = JS_UNDEFINED;
        { int k; for (k = 0; k < 5; k++) s->cb[k] = JS_UNDEFINED; }
        if (JS_IsUndefined(hdr->this_val))
            return JS_ThrowTypeError(ctx, "constructor TextDecoderStream requires 'new'"), -1;
        if (argc > 0 && !JS_IsUndefined(argv[0])) {
            label = JS_ToCStringLen(ctx, &label_len, argv[0]);
            if (!label) return -1;
        }
        id = encoding_lookup(label, label_len);
        if (argc > 0 && !JS_IsUndefined(argv[0])) JS_FreeCString(ctx, label);
        /* §7.5 step 2: failure AND `replacement` are both the RangeError — the replacement encoding exists to
           make a hostile label decode to one error rather than to something scriptable, and no constructor in
           this standard lets a page name it. */
        if (id < 0 || encoding_is_replacement(id))
            return JS_ThrowRangeError(ctx, "the label does not name a usable encoding"), -1;
        if (tc_build(ctx, s, true, id, idl_dict_bool(ctx, options, "fatal"),
                     idl_dict_bool(ctx, options, "ignoreBOM")) < 0)
            return -1;
        hdr->stage = TC_SETUP;
    }
    return tc_run(ctx, hdr, s, cb_result, presult, TC_DONE, out_cb, out_argc);
}

/* §7.6's constructor: no arguments at all — a TextEncoderStream is always UTF-8. */
static int js_tes_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSTcState *s = st;

    (void)argc; (void)argv;
    if (hdr->stage == TC_ENTRY) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->obj = JS_UNDEFINED;
        s->fns[0] = s->fns[1] = s->fns[2] = JS_UNDEFINED;
        { int k; for (k = 0; k < 5; k++) s->cb[k] = JS_UNDEFINED; }
        if (JS_IsUndefined(hdr->this_val))
            return JS_ThrowTypeError(ctx, "constructor TextEncoderStream requires 'new'"), -1;
        if (tc_build(ctx, s, false, 0, false, false) < 0) return -1;
        hdr->stage = TC_SETUP;
    }
    return tc_run(ctx, hdr, s, cb_result, presult, TC_DONE, out_cb, out_argc);
}

static const IdlStepDecl js_tds_ctor_decl = {
    js_tds_ctor_step, sizeof(JSTcState), js_tc_visit, NULL,
    "Encoding §7.5 new TextDecoderStream(label, options)", TC_STEPS
};
/* THE SAME STATE AND THE SAME STAGES — §7.5 and §7.6 differ in what they put in the algorithms and in nothing
   after that, which is why one struct, one visit and one stage list serve both. */
static const IdlStepDecl js_tes_ctor_decl = {
    js_tes_ctor_step, sizeof(JSTcState), js_tc_visit, NULL,
    "Encoding §7.6 new TextEncoderStream()", TC_STEPS
};

/* ---- the UTF-8 text decode two standards share --------------------------------------------------------------
 *
 * FETCH §5.3's `textStream()` STEPS 4-6 AND FILE API §3.3.6's STEPS 2-4 ARE THE SAME THREE STEPS, WORD FOR
 * WORD, which is why they are ONE operation here and not a copy in each caller. Fetch writes them "Let decoder
 * be a new TextDecoderStream object in this's relevant realm", "Set up decoder with UTF-8" and "Return the
 * result of stream, piped through decoder"; File API writes "Let decoder be a new TextDecoderStream in this's
 * relevant realm", "Set up decoder with UTF-8" and "Return the result of calling stream, piped through
 * decoder". What the two members do NOT share is their prologue — Fetch has an unusable check and a null-body
 * arm, File API has neither — and that stays with each member, where the spec puts it.
 *
 * IT LIVES HERE BECAUSE THE DECODER IS THE PART THAT MUST BE BUILT. "A new TextDecoderStream object" is Web IDL
 * "new", which CREATES an object without running the constructor, and "set up decoder with UTF-8" is Encoding
 * §7.5's own "set up a text decoder stream" — an operation this standard exports for exactly this use. Neither
 * is reachable through the page-visible `new TextDecoderStream()`: that constructor's steps 1-3 look up a LABEL
 * and refuse a call without `new`, and neither is a thing another standard's algorithm performs.
 *
 * THE PIPE IS ONE CALL AND NOT A SECOND IMPLEMENTATION — Streams §9.5 Piping's "piped through", taken from
 * core/streams/pipe.h, which is the operation another standard performs rather than §4.2.4's member. */

/* WHERE THIS MACHINE RESTS. Its first two stages ARE the constructor's above — the same create-and-set-up, so
   the same two numbers — and it adds one: the pipe. That alignment is what lets tc_run drive the middle of both,
   and it is asserted rather than assumed. */
#define TD_STAGES(X) \
    X(TD_ENTRY = IDL_STEP_FIRST, \
      "Encoding §7.5 set up a text decoder stream steps 1-8 (the assert that UTF-8 is not the replacement " \
      "encoding, the four slots, the decoder and I/O queue, and the two algorithms — one O(1) engine action, " \
      "no step of which reaches a page) — Fetch §5.3 textStream() step 4 / File API §3.3.6 textStream() step " \
      "2's new TextDecoderStream object") \
    X(TD_SETUP, \
      "Encoding §7.5 set up a text decoder stream steps 9-11 (a new TransformStream, Streams §9.3.1's set up " \
      "over those algorithms, and stream's transform) — Fetch §5.3 textStream() step 5 / File API §3.3.6 " \
      "textStream() step 3") \
    X(TD_PIPE, \
      "Streams §9.5 Piping's piped through — Fetch §5.3 textStream() step 6 / File API §3.3.6 textStream() " \
      "step 4 (the result of stream, piped through decoder)") \
    X(TD_DONE, \
      "Fetch §5.3 textStream() / File API §3.3.6 textStream() (the transform's readable half is the result)")
enum { TD_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TD_STEPS[] = { TD_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* tc_run ASSERTS `hdr->stage == TC_SETUP` and this machine reaches it at TD_SETUP, so the two enumerations
   agree or the assert is about a different number than the one the machine is at. Both lists open at
   IDL_STEP_FIRST with an entry stage and a set-up stage, which is the fact being pinned — a stage inserted
   into either list ahead of the set-up breaks it here rather than at whichever call first parked. */
_Static_assert((int)TD_ENTRY == (int)TC_ENTRY && (int)TD_SETUP == (int)TC_SETUP,
               "the text-decode machine's first two stages must be the text stream constructor's, because "
               "tc_run drives both and asserts the constructor's number");

typedef struct {
    /* FIRST, so js_tc_visit can walk it through this state's own pointer — one ownership list for the
       create-and-set-up half rather than a second copy of its five call slots and its three algorithms. */
    JSTcState tc;
    JSValue decoder;        /* what the set-up answered, until the pipe has been performed (owned) */
    uint8_t pipe_phase;     /* the §9.5 call's cursor — tc.phase is the set-up call's and they never overlap */
    JSValue pipe_cb[3];     /* step_call_run's buffer: [this, func, transform] */
} JSTdState;

static void js_td_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSTdState *s = st;
    int k;
    js_tc_visit(ctx, &s->tc, v);
    v->val(ctx, &s->decoder);
    for (k = 0; k < 3; k++) v->val(ctx, &s->pipe_cb[k]);
}

static int js_td_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                      JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSTdState *s = st;
    int r;

    (void)argc; (void)argv;
    if (hdr->stage == TD_ENTRY) {
        int enc;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* EVERY OWNED SLOT BEFORE THE FIRST OPERATION THAT CAN THROW. tc_build allocates and can fail, and the
           failure path tears this state down through the declaration's `visit` — which frees exactly what the
           state holds and nothing it never got, so a slot handed over late is freed uninitialised. The
           constructor half is initialised here rather than by tc_build for the same reason it is there. */
        s->tc.obj = JS_UNDEFINED;
        s->tc.fns[0] = s->tc.fns[1] = s->tc.fns[2] = JS_UNDEFINED;
        { int k; for (k = 0; k < 5; k++) s->tc.cb[k] = JS_UNDEFINED; }
        s->decoder = JS_UNDEFINED;
        s->pipe_cb[0] = s->pipe_cb[1] = s->pipe_cb[2] = JS_UNDEFINED;

        DCHECK(readable_stream_is(hdr->this_val),
               "the UTF-8 text decode was performed on a receiver that is not a ReadableStream — Fetch §5.3 "
               "step 3 and File API §3.3.6 step 1 each name one before this operation is reached, so a caller "
               "arriving with anything else skipped its own step");
        /* §7.5's set up a text decoder stream takes "an optional encoding encoding (default UTF-8)" and both
           callers say UTF-8 explicitly. Looked up in encoding.c's own table rather than written as a number,
           so this names the same encoding `new TextDecoderStream()` does. */
        enc = encoding_lookup("utf-8", sizeof("utf-8") - 1);
        DCHECK(enc >= 0 && !encoding_is_replacement(enc),
               "§7.5's set up a text decoder stream step 1 asserts \"encoding is not replacement\", and the "
               "UTF-8 label did not resolve to a usable encoding — the label table and this operation "
               "disagree about the one encoding every caller of it names");
        if (tc_build(ctx, &s->tc, /*decoder*/ true, enc, /*fatal*/ false, /*ignore_bom*/ false) < 0)
            return -1;
        hdr->stage = TD_SETUP;
    }

    if (hdr->stage == TD_SETUP) {
        r = tc_run(ctx, hdr, &s->tc, cb_result, &s->decoder, TD_PIPE, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;   /* tc_run's own request consumed it */
    }

    if (hdr->stage == TD_PIPE) {
        TextStreamData *t = ts_any_of(s->decoder);
        JSValue op, out;
        JSValueConst arg;

        DCHECK(t != NULL, "the UTF-8 text decode lost its own TextDecoderStream between two stages");
        /* §9.5's operand is a TransformStream, and a TextDecoderStream is NOT one — it INCLUDES Streams
           §9.3.2's GenericTransformStream mixin, whose associated `transform` is the transform stream. Fetch
           and File API both write "piped through decoder" naming the including object, and the mixin is what
           that resolves through; handing the decoder itself to §9.5 would fail its own brand test. */
        arg = t->transform;
        DCHECK(transform_stream_is(arg),
               "a TextDecoderStream reached the pipe with no TransformStream in its mixin slot — set up is "
               "what fills it and this stage is only reached once set up has answered");
        op = pipe_through_op(ctx);
        r = step_call_run(ctx, &s->pipe_phase, STEP_CB(s->pipe_cb), op, hdr->this_val, 1, &arg,
                          cb_result, &out, out_cb, out_argc);
        JS_FreeValue(ctx, op);
        if (r > 0) return r;
        if (JS_IsException(out)) return -1;
        DCHECK(readable_stream_is(out),
               "Streams §9.5's piped through answered something that is not a ReadableStream — its last step "
               "is \"Return transform.[[readable]]\", which is one by construction");
        STEP_GOTO(hdr->stage, TD_DONE, &s->pipe_phase, &s->tc.phase, NULL);
        *presult = out;
        return 0;
    }

    DFAIL("the UTF-8 text decode resumed at a stage Fetch §5.3 and File API §3.3.6 do not have between them");
    JS_FreeValue(ctx, cb_result);
    return -1;
}

static const IdlStepDecl js_td_decl = {
    js_td_step, sizeof(JSTdState), js_td_visit, NULL,
    "Fetch §5.3 textStream() steps 4-6 / File API §3.3.6 textStream() steps 2-4", TD_STEPS,
    /* `catches_abrupt` = 0: a throw from the set-up or from the pipe belongs to whichever member called this,
       and through it to the page. `unforkable` = NULL: the state is JSValues the visit names and nothing
       else — no lexbor handle, no foreign allocation — so a fork copies it whole. */
    0, NULL
};

static int g_td_stepid = -1;
/* THIS REALM'S copy of the operation — a function object carries the realm it was minted in, so one held in a
   module static would decode every document's body in whichever realm first asked. */
static int g_td_slot = -1;

/* ---- install -------------------------------------------------------------------------------------------- */

static const IdlDictMember DECODER_OPTIONS[] = {
    { "fatal",     IDL_BOOLEAN },
    { "ignoreBOM", IDL_BOOLEAN },
};

void text_stream_init(JSContext *ctx)
{
    JSClassDef dsd = { "TextDecoderStream", .finalizer = text_stream_finalizer,
                       .gc_mark = text_stream_gc_mark };
    JSClassDef esd = { "TextEncoderStream", .finalizer = text_stream_finalizer,
                       .gc_mark = text_stream_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType TDS_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
    int i;

    DCHECK(g_ts_rt == NULL || g_ts_rt == rt, "the Encoding stream interfaces were installed into a second "
                                             "runtime — one WASM instance is one document");
    if (g_ts_rt == rt) return;
    g_ts_rt = rt;
    JS_NewClassID(rt, &g_tds_class); JS_NewClass(rt, g_tds_class, &dsd);
    JS_NewClassID(rt, &g_tes_class); JS_NewClass(rt, g_tes_class, &esd);

    for (i = 0; i < ALG_N; i++) {
        g_alg_stepid[i] = JS_RegisterStepDef(rt, &js_tx_defs[i]);
        CHECK(g_alg_stepid[i] >= 0, "encoding: no step id for a streaming algorithm");
    }


    g_tds_ctor_stepid = idl_method_id_step(ctx, TDS_CTOR_ARGS, 2, DECODER_OPTIONS,
                                           (int)(sizeof DECODER_OPTIONS / sizeof DECODER_OPTIONS[0]),
                                           &js_tds_ctor_decl, 0);
    idl_optional_from(0);   /* §7.5: both constructor arguments are optional */
    g_tes_ctor_stepid = idl_method_id_step(ctx, NULL, 0, NULL, 0, &js_tes_ctor_decl, 0);
    /* NO ARGUMENTS: the stream is the RECEIVER, because §9.5's piped-through is defined on one and this
       operation is the caller that performs it. */
    g_td_stepid = idl_method_id_step(ctx, NULL, 0, NULL, 0, &js_td_decl, 0);
    g_td_slot = realm_value_declare(ctx, "Encoding §7.5's UTF-8 text decode of a ReadableStream");
    realm_declare_intrinsic(text_stream_install_protos);
}

/* §7.5's AND §7.6's INTERFACE PROTOTYPE OBJECTS, FOR ONE REALM. */
void text_stream_install_protos(JSContext *ctx)
{
    JSValue tds_p, tes_p, prev;

    DCHECK(g_tds_class != 0, "a realm asked for TextDecoderStream.prototype before the class was declared");
    prev = JS_GetClassProto(ctx, g_tds_class);
    DCHECK(JS_IsNull(prev), "text_stream_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);

    tds_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(tds_p), "TextDecoderStream.prototype could not be allocated");
    idl_interface_tag(ctx, tds_p, "TextDecoderStream");
    idl_install_accessor(ctx, tds_p, "encoding", js_tds_get, TDS_ENCODING, -1);
    idl_install_accessor(ctx, tds_p, "fatal", js_tds_get, TDS_FATAL, -1);
    idl_install_accessor(ctx, tds_p, "ignoreBOM", js_tds_get, TDS_IGNORE_BOM, -1);
    idl_install_accessor(ctx, tds_p, "readable", js_gts_get, GTS_READABLE, -1);
    idl_install_accessor(ctx, tds_p, "writable", js_gts_get, GTS_WRITABLE, -1);
    JS_SetClassProto(ctx, g_tds_class, tds_p);

    tes_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(tes_p), "TextEncoderStream.prototype could not be allocated");
    idl_interface_tag(ctx, tes_p, "TextEncoderStream");
    idl_install_accessor(ctx, tes_p, "encoding", js_tes_get, 0, -1);
    idl_install_accessor(ctx, tes_p, "readable", js_gts_get, GTS_READABLE, -1);
    idl_install_accessor(ctx, tes_p, "writable", js_gts_get, GTS_WRITABLE, -1);
    JS_SetClassProto(ctx, g_tes_class, tes_p);

    /* The shared decode, minted for THIS realm. It is not a member of either interface, so it goes on neither
       prototype and no page can reach it; the two `textStream()` members take it with text_stream_decode_op. */
    {
        DCHECK(g_td_stepid >= 0,
               "a realm asked for the UTF-8 text decode before text_stream_init declared its machine");
        realm_value_set(ctx, g_td_slot, idl_step_function(ctx, "textStream", g_td_stepid));
    }
}

JSValue text_stream_decode_op(JSContext *ctx)
{
    DCHECK(g_td_slot >= 0,
           "the UTF-8 text decode was asked for before this component declared its realm slot");
    return realm_value_get(ctx, g_td_slot);   /* OWNED */
}

void text_stream_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_tds_ctor_stepid >= 0,
           "the Encoding stream globals were installed before text_stream_init declared them");
    ctor = idl_step_constructor(ctx, "TextDecoderStream", g_tds_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the TextDecoderStream interface object could not be allocated");
    {
        JSValue proto = JS_GetClassProto(ctx, g_tds_class);
        DCHECK(!JS_IsNull(proto), "TextDecoderStream was installed into a realm with no proto build");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    idl_define_global_property_reference(ctx, global, "TextDecoderStream", ctor);

    ctor = idl_step_constructor(ctx, "TextEncoderStream", g_tes_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the TextEncoderStream interface object could not be allocated");
    {
        JSValue proto = JS_GetClassProto(ctx, g_tes_class);
        DCHECK(!JS_IsNull(proto), "TextEncoderStream was installed into a realm with no proto build");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    idl_define_global_property_reference(ctx, global, "TextEncoderStream", ctor);
}

void text_stream_free(JSContext *ctx)
{
    int i;
    if (!g_ts_rt) return;
    /* the prototypes are the REALMS' — released with their contexts */
    g_ts_rt = NULL;
    g_tds_ctor_stepid = g_tes_ctor_stepid = -1;
    g_td_stepid = -1;
    /* The realm's own copy went back with its context; what this component owns is the HANDLE, and one carried
       into the next runtime would name a slot that runtime never set. */
    g_td_slot = -1;
    for (i = 0; i < ALG_N; i++) g_alg_stepid[i] = -1;
}
