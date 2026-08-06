/* TextEncoder AND TextDecoder — the Encoding Standard §7.
 *
 * WHY IT IS BUILT NOW. It is what twelve of the remaining wpt Blob failures name, and it is what a bundle
 * reaches for the moment it touches bytes — `new TextDecoder().decode(await res.arrayBuffer())` is the ordinary
 * way to read a binary reply, and a page that calls it against an absent global aborts its own boot.
 *
 * THE DECODERS ARE THE STANDARD'S, STATE MACHINE BY STATE MACHINE. §4.4's UTF-8 decoder is written here as the
 * three-variable machine the standard writes (bytes-needed, bytes-seen, and the lower/upper bound the NEXT byte
 * must fall in) rather than as a switch over sequence lengths — because that is what makes an overlong
 * encoding, a surrogate encoded in three bytes, and a truncated sequence at the end of a STREAMING chunk each
 * come out as the error the standard says and not as a code point.
 *
 * WHAT IS ABSENT. The multi-byte CJK decoders (Big5, EUC-JP, EUC-KR, gb18030, ISO-2022-JP, Shift_JIS) each need
 * their own algorithm over a hundreds-of-kilobyte index. Their LABELS resolve — `new TextDecoder("shift_jis")`
 * is a known encoding and not a RangeError, which is a distinction §4.2 makes and this table keeps — and the
 * decode CRASHES naming the encoding, which is the work queue rather than a wrong answer. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/encoding/encoding.h"
#include "core/encoding/encoding_table.h"
#include "core/idl_args.h"

/* ---- §4.2's "get an encoding" ------------------------------------------------------------------------------
 *
 * Strip leading and trailing ASCII whitespace, ASCII-lowercase, and look the result up. The table is sorted, so
 * this is a binary search rather than 228 comparisons per call. Returns -1 for failure, which is the page's
 * RangeError. */
static int encoding_get(const char *label, size_t len)
{
    char buf[64];
    size_t i, n = 0;
    int lo = 0, hi = ENCODING_LABEL_N - 1;

    while (len && (label[0] == 0x09 || label[0] == 0x0A || label[0] == 0x0C || label[0] == 0x0D ||
                   label[0] == 0x20)) { label++; len--; }
    while (len && (label[len - 1] == 0x09 || label[len - 1] == 0x0A || label[len - 1] == 0x0C ||
                   label[len - 1] == 0x0D || label[len - 1] == 0x20)) len--;
    /* The longest label the standard lists is 19 characters; anything longer cannot match, and refusing it
       here is what keeps this buffer a fixed size rather than an allocation per lookup. */
    if (len >= sizeof buf) return -1;
    for (i = 0; i < len; i++)
        buf[n++] = (label[i] >= 'A' && label[i] <= 'Z') ? (char)(label[i] - 'A' + 'a') : label[i];
    buf[n] = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        /* COMPARED WITH ITS LENGTH, not as a C string. A label may contain a NUL — wpt tests every one of the
           228 with U+0000 prepended, appended and both — and `strcmp` stops there, so `"utf-8\0"` compared
           EQUAL to `utf-8` and constructed a decoder where the standard has a RangeError. The ordering is
           still strcmp's, which is what the sorted table was built with: compare the common prefix, and the
           shorter string sorts first. */
        size_t tlen = strlen(ENCODING_LABELS[mid].label);
        size_t common = n < tlen ? n : tlen;
        int c = common ? memcmp(buf, ENCODING_LABELS[mid].label, common) : 0;
        if (c == 0) c = n < tlen ? -1 : (n > tlen ? 1 : 0);
        if (c == 0) return (int)ENCODING_LABELS[mid].id;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

/* ---- the output buffer -------------------------------------------------------------------------------------
 *
 * Every decoder emits SCALAR VALUES — the standard's error handling turns a lone surrogate into U+FFFD before
 * it can reach the output — so the result is always well-formed UTF-8 and JS_NewStringLen is the right way to
 * hand it over. */
typedef struct { char *b; size_t n, cap; } EncBuf;

static void enc_put(EncBuf *o, uint32_t cp)
{
    if (o->n + 4 > o->cap) {
        char *g = realloc(o->b, o->cap = o->cap ? o->cap * 2 : 64);
        CHECK(g, "encoding: OOM decoding");
        o->b = g;
    }
    if (cp < 0x80) {
        o->b[o->n++] = (char)cp;
    } else if (cp < 0x800) {
        o->b[o->n++] = (char)(0xC0 | (cp >> 6));
        o->b[o->n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o->b[o->n++] = (char)(0xE0 | (cp >> 12));
        o->b[o->n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o->b[o->n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        o->b[o->n++] = (char)(0xF0 | (cp >> 18));
        o->b[o->n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        o->b[o->n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o->b[o->n++] = (char)(0x80 | (cp & 0x3F));
    }
}

/* ---- §7.1's TextDecoder ------------------------------------------------------------------------------------ */

/* THE DECODER'S STATE, which is what makes `stream: true` work: a sequence split across two calls resumes in
   the middle, and §7.1's "serialize stream" only flushes an incomplete one when the last chunk arrives. */
typedef struct {
    EncodingId enc;
    uint8_t fatal, ignore_bom, bom_seen;
    /* §4.4's UTF-8 machine: how many continuation bytes are still owed, how many have been seen, the code
       point so far, and the range the NEXT byte must fall in (which is what rejects an overlong sequence and a
       surrogate without a table of special cases). */
    uint32_t cp;
    uint8_t needed, seen, lo, hi;
    /* The UTF-16 machines' half-read unit and pending lead surrogate, for the same reason. */
    int16_t half;          /* -1 when no byte is held */
    int32_t lead_surrogate; /* -1 when none */
} DecoderState;

static JSClassID g_dec_class;
static JSClassID g_enc_class;
static JSValue   g_dec_proto = JS_UNDEFINED;
static JSValue   g_enc_proto = JS_UNDEFINED;
static JSRuntime *g_enc_rt;
static int       g_dec_ctor_stepid = -1, g_enc_ctor_stepid = -1;

static void decoder_finalizer(JSRuntime *rt, JSValue val)
{
    DecoderState *d = JS_GetOpaque(val, g_dec_class);
    (void)rt;
    free(d);
}

static void encoder_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt; (void)val;   /* a TextEncoder holds no state: §7.2 makes its encoding always UTF-8 */
}

static void decoder_reset(DecoderState *d)
{
    d->cp = 0;
    d->needed = d->seen = 0;
    d->lo = 0x80;
    d->hi = 0xBF;
    d->half = -1;
    d->lead_surrogate = -1;
    d->bom_seen = 0;
}

/* §4.4's UTF-8 decoder, byte by byte. Returns 0 on success; -1 means the byte was an ERROR, and `*prepeat` says
   the byte must be reprocessed after it (which is how the standard recovers from a truncated sequence without
   swallowing the byte that ended it). */
static int utf8_step(DecoderState *d, uint8_t b, EncBuf *o, int *prepeat, int fatal)
{
    *prepeat = 0;
    if (d->needed == 0) {
        if (b <= 0x7F) { enc_put(o, b); return 0; }
        if (b >= 0xC2 && b <= 0xDF) { d->needed = 1; d->cp = b & 0x1F; return 0; }
        if (b >= 0xE0 && b <= 0xEF) {
            if (b == 0xE0) d->lo = 0xA0;         /* rejects the overlong three-byte forms */
            if (b == 0xED) d->hi = 0x9F;         /* rejects a surrogate encoded as three bytes */
            d->needed = 2;
            d->cp = b & 0x0F;
            return 0;
        }
        if (b >= 0xF0 && b <= 0xF4) {
            if (b == 0xF0) d->lo = 0x90;
            if (b == 0xF4) d->hi = 0x8F;         /* rejects anything above U+10FFFF */
            d->needed = 3;
            d->cp = b & 0x07;
            return 0;
        }
        if (!fatal) enc_put(o, 0xFFFD);
        return -1;
    }
    if (b < d->lo || b > d->hi) {
        /* The sequence is broken. The standard RESTORES the machine and reprocesses this byte, so a lead byte
           that interrupts a sequence starts its own rather than being eaten by the error. */
        d->cp = 0;
        d->needed = d->seen = 0;
        d->lo = 0x80;
        d->hi = 0xBF;
        *prepeat = 1;
        if (!fatal) enc_put(o, 0xFFFD);
        return -1;
    }
    d->lo = 0x80;
    d->hi = 0xBF;
    d->cp = (d->cp << 6) | (b & 0x3F);
    d->seen++;
    if (d->seen != d->needed) return 0;
    enc_put(o, d->cp);
    d->cp = 0;
    d->needed = d->seen = 0;
    return 0;
}

/* §14.4/§14.5's UTF-16 decoders. One machine, `be` deciding the byte order — they differ in nothing else. */
static int utf16_step(DecoderState *d, uint8_t b, EncBuf *o, int *prepeat, int fatal, int be)
{
    uint32_t unit;

    *prepeat = 0;
    if (d->half < 0) { d->half = b; return 0; }
    unit = be ? (uint32_t)(((uint32_t)d->half << 8) | b) : (uint32_t)(((uint32_t)b << 8) | (uint32_t)d->half);
    d->half = -1;
    if (d->lead_surrogate >= 0) {
        int32_t lead = d->lead_surrogate;
        d->lead_surrogate = -1;
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            enc_put(o, 0x10000 + ((uint32_t)(lead - 0xD800) << 10) + (unit - 0xDC00));
            return 0;
        }
        /* A lead with no trail: the standard emits an error for the LEAD and reprocesses these two bytes. */
        *prepeat = 2;
        if (!fatal) enc_put(o, 0xFFFD);
        return -1;
    }
    if (unit >= 0xD800 && unit <= 0xDBFF) { d->lead_surrogate = (int32_t)unit; return 0; }
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
        if (!fatal) enc_put(o, 0xFFFD);
        return -1;
    }
    enc_put(o, unit);
    return 0;
}

/* Run `len` bytes through the receiver's decoder. Returns -1 with a TypeError live in fatal mode. */
static int decoder_run(JSContext *ctx, DecoderState *d, const uint8_t *p, size_t len, EncBuf *o)
{
    int row = ENCODING_SINGLE_BYTE_ROW[d->enc];
    size_t i;

    for (i = 0; i < len; i++) {
        uint8_t b = p[i];
        int repeat = 0, err = 0;

        if (d->enc == ENC_UTF_8) {
            err = utf8_step(d, b, o, &repeat, d->fatal);
        } else if (d->enc == ENC_UTF_16LE || d->enc == ENC_UTF_16BE) {
            err = utf16_step(d, b, o, &repeat, d->fatal, d->enc == ENC_UTF_16BE);
            if (repeat == 2) {
                /* BOTH BYTES of the orphaned unit are reprocessed. The standard emits the error for the LEAD
                   surrogate and then restarts on the unit that followed it, so a lead followed by U+0000 is
                   U+FFFD then U+0000 — re-reading only the second byte made it U+FFFD then U+FFFD, because
                   the first byte of that unit had already been eaten. */
                DCHECK(i >= 1, "the UTF-16 decoder orphaned a lead surrogate with fewer than two bytes read");
                d->half = -1;
                i -= 2;
                repeat = 0;
            }
        } else if (d->enc == ENC_X_USER_DEFINED) {
            /* §14.6: a byte below 0x80 is itself, and everything else maps into the private use area. */
            enc_put(o, b < 0x80 ? b : (uint32_t)(0xF780 + b - 0x80));
        } else if (row >= 0) {
            /* §9.1: below 0x80 is ASCII, and above it is one index lookup. A 0 entry is not U+0000 — the
               standard writes an absent pointer as a missing ROW, and an absent pointer is an error. */
            if (b < 0x80) {
                enc_put(o, b);
            } else {
                uint16_t cp = ENCODING_SINGLE_BYTE[row][b - 0x80];
                if (cp) enc_put(o, cp);
                else { if (!d->fatal) enc_put(o, 0xFFFD); err = -1; }
            }
        } else {
            DFAIL("a page asked to decode a legacy multi-byte encoding — build its decoder: each of Big5, "
                  "EUC-JP, EUC-KR, gb18030, ISO-2022-JP and Shift_JIS is its own algorithm over its own index, "
                  "and the label already resolves so only the decode is missing");
        }
        if (err && d->fatal) {
            JS_ThrowTypeError(ctx, "the encoded data was not valid %s", ENCODING_NAMES[d->enc]);
            return -1;
        }
        if (repeat) i--;
    }
    return 0;
}

enum { DEC_ENCODING = 0, DEC_FATAL, DEC_IGNORE_BOM };

static JSValue js_decoder_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    DecoderState *d = JS_GetOpaque(this_val, g_dec_class);
    if (!d) return JS_ThrowTypeError(ctx, "not a TextDecoder");
    if (magic == DEC_ENCODING) return JS_NewString(ctx, ENCODING_NAMES[d->enc]);
    if (magic == DEC_FATAL) return JS_NewBool(ctx, d->fatal);
    DCHECK(magic == DEC_IGNORE_BOM, "a TextDecoder attribute was declared with a magic this component does not "
                                    "answer");
    return JS_NewBool(ctx, d->ignore_bom);
}

/* THE BYTES OF A BufferSource the declaration has already brand-tested, or -1. `*pbuf` is a value the caller
   frees. */
static int enc_buffer_source(JSContext *ctx, JSValueConst v, const uint8_t **pp, size_t *plen, JSValue *pbuf)
{
    size_t off = 0, whole = 0;
    uint8_t *base;

    *pbuf = JS_UNDEFINED;
    if (JS_IsArrayBuffer(v)) {
        *pp = JS_GetArrayBuffer(ctx, plen, (JSValue)v);
        return *pp ? 0 : -1;
    }
    DCHECK(JS_GetTypedArrayType(v) >= 0 || JS_IsDataView(v),
           "a BufferSource argument reached its body as neither an ArrayBuffer nor a view — the declaration "
           "brand-tests the union, so nothing else can arrive here");
    *pbuf = JS_GetArrayBufferView(ctx, v, &off, plen);
    if (JS_IsException(*pbuf)) return -1;
    base = JS_GetArrayBuffer(ctx, &whole, *pbuf);
    if (!base) { JS_FreeValue(ctx, *pbuf); *pbuf = JS_UNDEFINED; return -1; }
    *pp = base + off;
    return 0;
}

/* §7.1's decode(). The `stream` flag decides whether an incomplete sequence at the end is HELD for the next
   call or flushed as an error, which is the whole reason a decoder has state at all. */
static JSValue js_decoder_decode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    DecoderState *d = JS_GetOpaque(this_val, g_dec_class);
    JSValueConst options = argc > 1 ? argv[1] : JS_UNDEFINED;
    const uint8_t *p = NULL;
    size_t len = 0, start = 0;
    JSValue buf = JS_UNDEFINED, r;
    EncBuf o = { 0 };
    bool stream;

    (void)magic;
    if (!d) return JS_ThrowTypeError(ctx, "not a TextDecoder");
    if (argc > 0 && !JS_IsUndefined(argv[0]) && enc_buffer_source(ctx, argv[0], &p, &len, &buf) < 0)
        return JS_EXCEPTION;
    stream = idl_dict_bool(ctx, options, "stream");

    if (decoder_run(ctx, d, p + start, len - start, &o) < 0) {
        free(o.b);
        JS_FreeValue(ctx, buf);
        decoder_reset(d);
        return JS_EXCEPTION;
    }
    if (!stream) {
        /* THE FLUSH. An incomplete sequence held across the last chunk is an error here — a stream that ends
           mid-character is not a character, and holding it silently would drop bytes the page handed over. */
        int incomplete = d->needed != 0 || d->half >= 0 || d->lead_surrogate >= 0;
        if (incomplete) {
            if (d->fatal) {
                free(o.b);
                JS_FreeValue(ctx, buf);
                decoder_reset(d);
                return JS_ThrowTypeError(ctx, "the encoded data ended mid-sequence");
            }
            enc_put(&o, 0xFFFD);
        }
        decoder_reset(d);
    }
    /* §7.1's BOM REMOVAL IS OVER THE DECODED OUTPUT, not over the input bytes — the standard drops the first
       SCALAR VALUE when it is U+FEFF and sets the flag on the first value either way. Written as a byte-prefix
       test it was wrong for streaming: a BOM split across two chunks (`EF BB` then `BF 40`) has no three-byte
       prefix to match in either call, so it decoded as a visible U+FEFF. The output is UTF-8, so U+FEFF is
       the three bytes this drops. */
    if (!d->ignore_bom && !d->bom_seen && o.n) {
        if (o.n >= 3 && (unsigned char)o.b[0] == 0xEF && (unsigned char)o.b[1] == 0xBB &&
            (unsigned char)o.b[2] == 0xBF) {
            memmove(o.b, o.b + 3, o.n - 3);
            o.n -= 3;
        }
        d->bom_seen = 1;
    }
    r = JS_NewStringLen(ctx, o.b ? o.b : "", o.n);
    free(o.b);
    JS_FreeValue(ctx, buf);
    return r;
}

/* §7.1's constructor. `label` resolving to `replacement` is a RangeError as much as an unknown label is — the
   replacement encoding exists to make a hostile label decode to one error rather than to something scriptable,
   and the standard refuses to let a page name it. */
typedef struct { uint8_t unused; } JSDecoderCtorState;

static void js_dec_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }
static void js_dec_ctor_release(JSContext *ctx, void *st) { (void)ctx; (void)st; }

static int js_dec_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSValueConst options = argc > 1 ? argv[1] : JS_UNDEFINED;
    const char *label = "utf-8";
    size_t label_len = 5;
    int id;
    DecoderState *d;
    JSValue obj;

    (void)st; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    if (JS_IsUndefined(hdr->this_val))
        return JS_ThrowTypeError(ctx, "constructor TextDecoder requires 'new'"), -1;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        label = JS_ToCStringLen(ctx, &label_len, argv[0]);
        if (!label) return -1;
    }
    id = encoding_get(label, label_len);
    if (argc > 0 && !JS_IsUndefined(argv[0])) JS_FreeCString(ctx, label);
    if (id < 0 || id == ENC_REPLACEMENT)
        return JS_ThrowRangeError(ctx, "the label does not name a usable encoding"), -1;

    obj = JS_NewObjectProtoClass(ctx, g_dec_proto, g_dec_class);
    if (JS_IsException(obj)) return -1;
    d = calloc(1, sizeof *d);
    CHECK(d, "encoding: OOM building a TextDecoder");
    d->enc = (EncodingId)id;
    d->fatal = idl_dict_bool(ctx, options, "fatal");
    d->ignore_bom = idl_dict_bool(ctx, options, "ignoreBOM");
    decoder_reset(d);
    JS_SetOpaque(obj, d);
    *presult = obj;
    return 0;
}

static const IdlStepDecl js_dec_ctor_decl = {
    js_dec_ctor_step, sizeof(JSDecoderCtorState), js_dec_ctor_visit, js_dec_ctor_release
};

/* ---- §7.2's TextEncoder ------------------------------------------------------------------------------------
 *
 * Always UTF-8, by the standard rather than by simplification: §7.2 gives the constructor no arguments and
 * fixes `encoding` at "utf-8", because a page that wants other bytes is meant to build them itself. */

static JSValue js_encoder_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!JS_GetOpaque(this_val, g_enc_class) && !JS_IsObject(this_val))
        return JS_ThrowTypeError(ctx, "not a TextEncoder");
    return JS_NewString(ctx, "utf-8");
}

static JSValue js_encoder_encode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    size_t len = 0;
    const char *s;
    JSValue r;

    (void)magic;
    if (JS_GetOpaque(this_val, g_enc_class) == NULL && !JS_IsObject(this_val))
        return JS_ThrowTypeError(ctx, "not a TextEncoder");
    /* The declaration already ran the USVString conversion, so lone surrogates are U+FFFD and this is the
       string's UTF-8 — which is exactly what §7.2 says to answer with. */
    s = argc > 0 && !JS_IsUndefined(argv[0]) ? JS_ToCStringLen(ctx, &len, argv[0]) : NULL;
    r = JS_NewUint8ArrayCopy(ctx, (const uint8_t *)(s ? s : ""), s ? len : 0);
    if (s) JS_FreeCString(ctx, s);
    return r;
}

/* §7.2's encodeInto(). It writes into the page's own buffer and reports how far it got, and the report is the
   whole point: it must never write a PARTIAL code unit sequence, so the loop stops at the last whole one. */
static JSValue js_encoder_encode_into(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    size_t len = 0, dlen = 0, off = 0, whole = 0, w = 0, i = 0;
    const char *s;
    uint8_t *dst;
    JSValue view, r;
    uint64_t read = 0;

    (void)magic;
    if (JS_GetOpaque(this_val, g_enc_class) == NULL && !JS_IsObject(this_val))
        return JS_ThrowTypeError(ctx, "not a TextEncoder");
    /* §7.2 declares the destination as `Uint8Array`, not as a BufferSource — an Int8Array or a Float32Array is
       a TypeError, because `written` counts BYTES and a view with a wider element would report a length in
       units the caller cannot use. "Is it a typed array" accepted all of them. */
    if (argc < 2 || JS_GetTypedArrayType(argv[1]) != JS_TYPED_ARRAY_UINT8)
        return JS_ThrowTypeError(ctx, "the destination is not a Uint8Array");
    view = JS_GetArrayBufferView(ctx, argv[1], &off, &dlen);
    if (JS_IsException(view)) return JS_EXCEPTION;
    dst = JS_GetArrayBuffer(ctx, &whole, view);
    if (!dst) { JS_FreeValue(ctx, view); return JS_EXCEPTION; }
    dst += off;
    s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) { JS_FreeValue(ctx, view); return JS_EXCEPTION; }

    while (i < len) {
        /* One whole UTF-8 sequence at a time, so a destination that ends mid-sequence gets none of it. */
        size_t seq = 1;
        unsigned char c = (unsigned char)s[i];
        if (c >= 0xF0) seq = 4; else if (c >= 0xE0) seq = 3; else if (c >= 0xC0) seq = 2;
        if (w + seq > dlen) break;
        memcpy(dst + w, s + i, seq);
        w += seq;
        i += seq;
        /* `read` counts UTF-16 code UNITS, which is what the page's string is measured in — a sequence of four
           UTF-8 bytes is one code point and TWO units. */
        read += seq == 4 ? 2 : 1;
    }
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, view);
    r = JS_NewObject(ctx);
    if (JS_IsException(r)) return r;
    JS_SetPropertyStr(ctx, r, "read", JS_NewInt64(ctx, (int64_t)read));
    JS_SetPropertyStr(ctx, r, "written", JS_NewInt64(ctx, (int64_t)w));
    return r;
}

typedef struct { uint8_t unused; } JSEncoderCtorState;

static void js_enc_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }
static void js_enc_ctor_release(JSContext *ctx, void *st) { (void)ctx; (void)st; }

static int js_enc_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    (void)st; (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    if (JS_IsUndefined(hdr->this_val))
        return JS_ThrowTypeError(ctx, "constructor TextEncoder requires 'new'"), -1;
    *presult = JS_NewObjectProtoClass(ctx, g_enc_proto, g_enc_class);
    return JS_IsException(*presult) ? -1 : 0;
}

static const IdlStepDecl js_enc_ctor_decl = {
    js_enc_ctor_step, sizeof(JSEncoderCtorState), js_enc_ctor_visit, js_enc_ctor_release
};

/* ---- install ---------------------------------------------------------------------------------------------- */

static const IdlDictMember DECODER_OPTIONS[] = {
    { "fatal",     IDL_BOOLEAN },
    { "ignoreBOM", IDL_BOOLEAN },
};
static const IdlDictMember DECODE_OPTIONS[] = {
    { "stream", IDL_BOOLEAN },
};

void encoding_init(JSContext *ctx)
{
    JSClassDef dec_def = { "TextDecoder", .finalizer = decoder_finalizer };
    JSClassDef enc_def = { "TextEncoder", .finalizer = encoder_finalizer };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType DEC_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
    static const IdlArgType DECODE_ARGS[2] = { IDL_BUFFERSOURCE, IDL_DICT };
    static const IdlArgType ENCODE_ARGS[1] = { IDL_USVSTRING };
    static const IdlArgType ENCODE_INTO_ARGS[2] = { IDL_USVSTRING, IDL_ANY };

    DCHECK(g_enc_rt == NULL || g_enc_rt == rt,
           "the Encoding components were installed into a second runtime — their class ids and step ids belong "
           "to the first, and one WASM instance is one document");
    if (g_enc_rt == rt)
        return;
    g_enc_rt = rt;
    JS_NewClassID(rt, &g_dec_class);
    JS_NewClass(rt, g_dec_class, &dec_def);
    JS_NewClassID(rt, &g_enc_class);
    JS_NewClass(rt, g_enc_class, &enc_def);

    g_dec_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_dec_proto), "TextDecoder.prototype could not be allocated");
    idl_interface_tag(ctx, g_dec_proto, "TextDecoder");
    idl_install_accessor(ctx, g_dec_proto, "encoding", js_decoder_get, DEC_ENCODING, -1);
    idl_install_accessor(ctx, g_dec_proto, "fatal", js_decoder_get, DEC_FATAL, -1);
    idl_install_accessor(ctx, g_dec_proto, "ignoreBOM", js_decoder_get, DEC_IGNORE_BOM, -1);
    idl_install_method(ctx, g_dec_proto, "decode", 0,
                       idl_method_id_dict(ctx, DECODE_ARGS, 2, DECODE_OPTIONS,
                                          (int)(sizeof DECODE_OPTIONS / sizeof DECODE_OPTIONS[0]),
                                          js_decoder_decode, 0));
    idl_optional_from(0);   /* §7.1: `decode(optional BufferSource input, optional TextDecodeOptions options)` */

    g_enc_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_enc_proto), "TextEncoder.prototype could not be allocated");
    idl_interface_tag(ctx, g_enc_proto, "TextEncoder");
    idl_install_accessor(ctx, g_enc_proto, "encoding", js_encoder_get, 0, -1);
    idl_install_method(ctx, g_enc_proto, "encode", 0,
                       idl_method_id(ctx, ENCODE_ARGS, 1, js_encoder_encode, 0));
    idl_optional_from(0);   /* §7.2: `encode(optional USVString input = "")` */
    idl_install_method(ctx, g_enc_proto, "encodeInto", 2,
                       idl_method_id(ctx, ENCODE_INTO_ARGS, 2, js_encoder_encode_into, 0));

    g_dec_ctor_stepid = idl_method_id_step(ctx, DEC_CTOR_ARGS, 2, DECODER_OPTIONS,
                                           (int)(sizeof DECODER_OPTIONS / sizeof DECODER_OPTIONS[0]),
                                           &js_dec_ctor_decl, 0);
    idl_optional_from(0);   /* §7.1: `constructor(optional DOMString label = "utf-8", optional options = {})` */
    g_enc_ctor_stepid = idl_method_id_step(ctx, NULL, 0, NULL, 0, &js_enc_ctor_decl, 0);
}

void encoding_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;
    DCHECK(g_dec_ctor_stepid >= 0, "the Encoding globals were installed before encoding_init declared them");
    ctor = idl_step_constructor(ctx, "TextDecoder", 0, g_dec_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the TextDecoder interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_dec_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "TextDecoder", ctor);

    ctor = idl_step_constructor(ctx, "TextEncoder", 0, g_enc_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the TextEncoder interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_enc_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "TextEncoder", ctor);
}

void encoding_free(JSContext *ctx)
{
    if (!g_enc_rt)
        return;
    JS_FreeValue(ctx, g_dec_proto);
    JS_FreeValue(ctx, g_enc_proto);
    g_dec_proto = g_enc_proto = JS_UNDEFINED;
    g_enc_rt = NULL;
    g_dec_ctor_stepid = g_enc_ctor_stepid = -1;
}
