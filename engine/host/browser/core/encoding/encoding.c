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
#include "solver/cow.h"
#include "core/encoding/encoding.h"
#include "core/encoding/encoding_table.h"
#include "core/idl_args.h"
#include "core/realm.h"

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

/* §13.3.1's decoder MODES. They are the standard's own states, named as it names them. */
enum { JIS_ASCII = 0, JIS_ROMAN, JIS_KATAKANA, JIS_LEAD, JIS_TRAIL, JIS_ESC_START, JIS_ESC };

/* THE DECODER'S STATE, which is what makes `stream: true` work: a sequence split across two calls resumes in
   the middle, and §7.1's "serialize stream" only flushes an incomplete one when the last chunk arrives. */
typedef struct EncDecoder {
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
    /* §11.1.1's three held bytes. The standard names them gb18030 first/second/third and tests each against 0,
       so 0 IS the "nothing held" value rather than a separate flag — a byte of 0x00 never reaches them because
       the machine only holds bytes in 0x81..0xFE and 0x30..0x39. */
    uint8_t gb_first, gb_second, gb_third;
    /* §12/§13/§14's single held LEAD byte, and EUC-JP's jis0212 flag — the one bit that decides which of two
       indexes its pointer is looked up in. §13.3.1's ISO-2022-JP is the one with a state rather than a lead:
       its escape sequences switch the whole decoder between ASCII, Roman, Katakana and two-byte modes, and
       `jis_out` is the state it returns to when an escape turns out not to be one. */
    uint8_t mb_lead, jis0212;
    uint8_t jis_state, jis_out, jis_lead, jis_output_flag;
    /* THE STANDARD'S "prepend to the stream". Every decoder here has a step that re-reads a byte it has
       already taken — UTF-8 recovering from a truncated sequence, UTF-16 orphaning a lead surrogate, gb18030
       giving back a whole held sequence — and a byte's INDEX in the caller's buffer is the wrong way to say
       that: the byte may have come from a PREVIOUS decode() call, where there is no index to go back to. The
       three of them held it as `i--` / `i -= 2` and a DCHECK that a stream split would have fired. This is a
       queue, and it is where they push back to. Three bytes is gb18030's worst case, which is the platform's. */
    uint8_t back[3], nback;
} EncDecoder;

static JSClassID g_dec_class;
static JSClassID g_enc_class;
/* THE AGENT'S POOL ENTRIES — the OBJECTS they are installed as are each realm's. */
static int       g_id_decode = -1, g_id_encode = -1, g_id_encode_into = -1;
static JSRuntime *g_enc_rt;
static int       g_dec_ctor_stepid = -1, g_enc_ctor_stepid = -1;

static void decoder_finalizer(JSRuntime *rt, JSValue val)
{
    EncDecoder *d = JS_GetOpaque(val, g_dec_class);
    (void)rt;
    enc_decoder_free(d);
}

static void encoder_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt; (void)val;   /* a TextEncoder holds no state: §7.2 makes its encoding always UTF-8 */
}

static void decoder_reset(EncDecoder *d)
{
    d->cp = 0;
    d->needed = d->seen = 0;
    d->lo = 0x80;
    d->hi = 0xBF;
    d->half = -1;
    d->lead_surrogate = -1;
    d->bom_seen = 0;
    d->gb_first = d->gb_second = d->gb_third = 0;
    d->nback = 0;
    d->mb_lead = d->jis0212 = 0;
    d->jis_state = d->jis_out = JIS_ASCII;
    d->jis_lead = d->jis_output_flag = 0;
}

/* THE STANDARD'S "prepend to the stream", in order: the first byte pushed is the first re-read. */
static void dec_push_back(EncDecoder *d, uint8_t b)
{
    DCHECK(d->nback < sizeof d->back, "a decoder pushed back more bytes than the queue holds");
    d->back[d->nback++] = b;
}

/* §4.4's UTF-8 decoder, byte by byte. Returns 0 on success; -1 means the byte was an ERROR, and `*prepeat` says
   the byte must be reprocessed after it (which is how the standard recovers from a truncated sequence without
   swallowing the byte that ended it). */
static int utf8_step(EncDecoder *d, uint8_t b, EncBuf *o, int *prepeat, int fatal)
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
static int utf16_step(EncDecoder *d, uint8_t b, EncBuf *o, int *prepeat, int fatal, int be)
{
    uint32_t unit;
    uint8_t first;

    *prepeat = 0;
    if (d->half < 0) { d->half = b; return 0; }
    first = (uint8_t)d->half;
    unit = be ? (uint32_t)(((uint32_t)first << 8) | b) : (uint32_t)(((uint32_t)b << 8) | (uint32_t)first);
    d->half = -1;
    if (d->lead_surrogate >= 0) {
        int32_t lead = d->lead_surrogate;
        d->lead_surrogate = -1;
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            enc_put(o, 0x10000 + ((uint32_t)(lead - 0xD800) << 10) + (unit - 0xDC00));
            return 0;
        }
        /* A lead with no trail: the standard emits an error for the LEAD and reprocesses these two bytes —
           BOTH of them, which is why they are pushed back here, where they are still known. The caller used to
           un-index them instead, and a unit split across two decode() calls has no index to go back to. */
        dec_push_back(d, first);
        dec_push_back(d, b);
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

/* §5's "index gb18030 ranges code point". The ranges are walked in the standard's own order, which is what
   makes the two special cases in front of the walk the standard's rather than an optimisation: a pointer inside
   the hole between the BMP and the astral ranges maps to nothing, and 7457 is the one entry the ranges
   themselves get wrong. Returns 0 for "maps to nothing" — U+0000 is not a value this can answer. */
static uint32_t gb18030_ranges_code_point(uint32_t pointer)
{
    uint32_t offset = 0, cp_offset = 0;
    int i;

    if ((pointer > 39419 && pointer < 189000) || pointer > 1237575) return 0;
    if (pointer == 7457) return 0xE7C7;
    for (i = 0; i < ENCODING_GB18030_RANGES_N; i++) {
        if (ENCODING_GB18030_RANGES[i].pointer > pointer) break;
        offset = ENCODING_GB18030_RANGES[i].pointer;
        cp_offset = ENCODING_GB18030_RANGES[i].code_point;
    }
    return cp_offset + pointer - offset;
}

/* §11.1.1's gb18030 decoder, byte by byte — and the one in this file whose state is THREE held bytes, because
 * the four-byte form is a range lookup that cannot be recognised until its fourth byte.
 *
 * Bytes the machine held but then finds do not belong to a sequence are PUSHED BACK rather than swallowed, so
 * a truncated sequence followed by an ASCII byte emits an error AND that byte. Returns -1 for an error. */
static int gb18030_step(EncDecoder *d, uint8_t b, EncBuf *o, bool fatal)
{
    uint32_t pointer, cp;
    uint8_t lead, offset;

    if (d->gb_third != 0) {
        if (b < 0x30 || b > 0x39) {
            dec_push_back(d, d->gb_second);   /* §11.1.1 prepends second, third and this byte */
            dec_push_back(d, d->gb_third);
            dec_push_back(d, b);
            d->gb_first = d->gb_second = d->gb_third = 0;
            if (!fatal) enc_put(o, 0xFFFD);
            return -1;
        }
        pointer = ((uint32_t)(d->gb_first - 0x81)) * (10u * 126u * 10u)
                + ((uint32_t)(d->gb_second - 0x30)) * (10u * 126u)
                + ((uint32_t)(d->gb_third - 0x81)) * 10u
                + (uint32_t)(b - 0x30);
        d->gb_first = d->gb_second = d->gb_third = 0;
        cp = gb18030_ranges_code_point(pointer);
        if (!cp) { if (!fatal) enc_put(o, 0xFFFD); return -1; }
        enc_put(o, cp);
        return 0;
    }
    if (d->gb_second != 0) {
        if (b >= 0x81 && b <= 0xFE) { d->gb_third = b; return 0; }
        dec_push_back(d, d->gb_second);   /* §11.1.1 prepends second and this byte */
        dec_push_back(d, b);
        d->gb_first = d->gb_second = 0;
        if (!fatal) enc_put(o, 0xFFFD);
        return -1;
    }
    if (d->gb_first != 0) {
        if (b >= 0x30 && b <= 0x39) { d->gb_second = b; return 0; }
        lead = d->gb_first;
        d->gb_first = 0;
        offset = b < 0x7F ? 0x40 : 0x41;
        if ((b >= 0x40 && b <= 0x7E) || (b >= 0x80 && b <= 0xFE)) {
            pointer = ((uint32_t)(lead - 0x81)) * 190u + (uint32_t)(b - offset);
            if ((int)pointer < ENCODING_GB18030_N && ENCODING_GB18030[pointer]) {
                enc_put(o, ENCODING_GB18030[pointer]);
                return 0;
            }
        }
        /* §11.1.1: an ASCII byte that ended a truncated sequence is RE-READ, so `\x81a` is an error followed
           by `a` rather than an error that ate it. */
        if (b < 0x80) dec_push_back(d, b);
        if (!fatal) enc_put(o, 0xFFFD);
        return -1;
    }
    if (b < 0x80) { enc_put(o, b); return 0; }
    if (b == 0x80) { enc_put(o, 0x20AC); return 0; }   /* §11.1.1: the euro sign, which is not in the index */
    if (b >= 0x81 && b <= 0xFE) { d->gb_first = b; return 0; }
    if (!fatal) enc_put(o, 0xFFFD);
    return -1;
}

/* An index lookup that answers 0 for "maps to nothing", which is what every one of these indexes writes an
   absent pointer as — U+0000 is not an entry in any of them. `n` is the index's own length, so a pointer past
   its end is absent rather than a read off the end. */
#define ENC_INDEX_CP(tbl, n, ptr) ((ptr) < (uint32_t)(n) ? (uint32_t)(tbl)[(ptr)] : 0u)

/* §12.1.1's Big5 decoder. Four pointers answer TWO code points each — the standard writes them out rather than
   putting them in the index, because an index maps a pointer to one — so this is the only decoder here that
   emits twice from one step. */
static int big5_step(EncDecoder *d, uint8_t b, EncBuf *o, bool fatal)
{
    uint32_t pointer, cp;
    uint8_t lead, offset;

    if (d->mb_lead != 0) {
        lead = d->mb_lead;
        d->mb_lead = 0;
        offset = b < 0x7F ? 0x40 : 0x62;
        if ((b >= 0x40 && b <= 0x7E) || (b >= 0xA1 && b <= 0xFE)) {
            pointer = ((uint32_t)(lead - 0x81)) * 157u + (uint32_t)(b - offset);
            switch (pointer) {
            case 1133: enc_put(o, 0x00CA); enc_put(o, 0x0304); return 0;
            case 1134: enc_put(o, 0x00CA); enc_put(o, 0x030C); return 0;
            case 1135: enc_put(o, 0x00EA); enc_put(o, 0x0304); return 0;
            case 1136: enc_put(o, 0x00EA); enc_put(o, 0x030C); return 0;
            default: break;
            }
            cp = ENC_INDEX_CP(ENCODING_BIG5, ENCODING_BIG5_N, pointer);
            if (cp) { enc_put(o, cp); return 0; }
        }
        if (b < 0x80) dec_push_back(d, b);
        if (!fatal) enc_put(o, 0xFFFD);
        return -1;
    }
    if (b < 0x80) { enc_put(o, b); return 0; }
    if (b >= 0x81 && b <= 0xFE) { d->mb_lead = b; return 0; }
    if (!fatal) enc_put(o, 0xFFFD);
    return -1;
}

/* §13.2.1's EUC-JP decoder. Its 0x8F lead selects the OTHER index for the pair that follows, which is what
   `jis0212` carries: one flag, cleared by the step that reads it, exactly as the standard clears it. */
static int euc_jp_step(EncDecoder *d, uint8_t b, EncBuf *o, bool fatal)
{
    uint32_t pointer = 0, cp = 0;
    uint8_t lead;

    if (d->mb_lead == 0x8E && b >= 0xA1 && b <= 0xDF) {
        d->mb_lead = 0;
        enc_put(o, 0xFF61u - 0xA1u + b);   /* the half-width katakana block */
        return 0;
    }
    if (d->mb_lead == 0x8F && b >= 0xA1 && b <= 0xFE) {
        d->jis0212 = 1;
        d->mb_lead = b;
        return 0;
    }
    if (d->mb_lead != 0) {
        lead = d->mb_lead;
        d->mb_lead = 0;
        if (lead >= 0xA1 && lead <= 0xFE && b >= 0xA1 && b <= 0xFE) {
            pointer = ((uint32_t)(lead - 0xA1)) * 94u + (uint32_t)(b - 0xA1);
            cp = d->jis0212 ? ENC_INDEX_CP(ENCODING_JIS0212, ENCODING_JIS0212_N, pointer)
                            : ENC_INDEX_CP(ENCODING_JIS0208, ENCODING_JIS0208_N, pointer);
        }
        d->jis0212 = 0;
        if (b < 0xA1 || b > 0xFE) dec_push_back(d, b);
        if (cp) { enc_put(o, cp); return 0; }
        if (!fatal) enc_put(o, 0xFFFD);
        return -1;
    }
    if (b < 0x80) { enc_put(o, b); return 0; }
    if (b == 0x8E || b == 0x8F || (b >= 0xA1 && b <= 0xFE)) { d->mb_lead = b; return 0; }
    if (!fatal) enc_put(o, 0xFFFD);
    return -1;
}

/* §13.4.1's Shift_JIS decoder. Its pointers 8836..10715 are the private use area rather than an index entry,
   which is why that range is answered before the lookup rather than filled into the table. */
static int shift_jis_step(EncDecoder *d, uint8_t b, EncBuf *o, bool fatal)
{
    uint32_t pointer, cp;
    uint8_t lead, offset, lead_offset;

    if (d->mb_lead != 0) {
        lead = d->mb_lead;
        d->mb_lead = 0;
        offset = b < 0x7F ? 0x40 : 0x41;
        lead_offset = lead < 0xA0 ? 0x81 : 0xC1;
        if ((b >= 0x40 && b <= 0x7E) || (b >= 0x80 && b <= 0xFC)) {
            pointer = ((uint32_t)(lead - lead_offset)) * 188u + (uint32_t)(b - offset);
            if (pointer >= 8836 && pointer <= 10715) {
                enc_put(o, 0xE000u - 8836u + pointer);
                return 0;
            }
            cp = ENC_INDEX_CP(ENCODING_JIS0208, ENCODING_JIS0208_N, pointer);
            if (cp) { enc_put(o, cp); return 0; }
        }
        if (b < 0x80) dec_push_back(d, b);
        if (!fatal) enc_put(o, 0xFFFD);
        return -1;
    }
    if (b < 0x80 || b == 0x80) { enc_put(o, b); return 0; }
    if (b >= 0xA1 && b <= 0xDF) { enc_put(o, 0xFF61u - 0xA1u + b); return 0; }
    if ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) { d->mb_lead = b; return 0; }
    if (!fatal) enc_put(o, 0xFFFD);
    return -1;
}

/* §14.1.1's EUC-KR decoder — the simplest of the five: one lead, one pointer, one index. */
static int euc_kr_step(EncDecoder *d, uint8_t b, EncBuf *o, bool fatal)
{
    uint32_t pointer, cp = 0;
    uint8_t lead;

    if (d->mb_lead != 0) {
        lead = d->mb_lead;
        d->mb_lead = 0;
        if (b >= 0x41 && b <= 0xFE) {
            pointer = ((uint32_t)(lead - 0x81)) * 190u + (uint32_t)(b - 0x41);
            cp = ENC_INDEX_CP(ENCODING_EUC_KR, ENCODING_EUC_KR_N, pointer);
        }
        if (!cp && b < 0x80) dec_push_back(d, b);
        if (cp) { enc_put(o, cp); return 0; }
        if (!fatal) enc_put(o, 0xFFFD);
        return -1;
    }
    if (b < 0x80) { enc_put(o, b); return 0; }
    if (b >= 0x81 && b <= 0xFE) { d->mb_lead = b; return 0; }
    if (!fatal) enc_put(o, 0xFFFD);
    return -1;
}

/* §13.3.1's ISO-2022-JP decoder — the one encoding here that is a MODE MACHINE rather than a lead byte. Its
 * escape sequences switch the whole decoder, and an escape that turns out not to be one has to unwind to the
 * mode it interrupted, which is what `jis_out` holds. The `output` flag is the standard's own: an escape that
 * lands on the mode already in force with nothing emitted since is an error, which is how it rejects
 * `ESC ( B ESC ( B` while accepting the same pair around real text. */
static int iso2022jp_step(EncDecoder *d, uint8_t b, EncBuf *o, bool fatal)
{
    uint32_t pointer, cp;

    switch (d->jis_state) {
    case JIS_ASCII:
        if (b == 0x1B) { d->jis_state = JIS_ESC_START; return 0; }
        if (b < 0x80 && b != 0x0E && b != 0x0F) { d->jis_output_flag = 0; enc_put(o, b); return 0; }
        d->jis_output_flag = 0;
        if (!fatal) enc_put(o, 0xFFFD);
        return -1;
    case JIS_ROMAN:
        if (b == 0x1B) { d->jis_state = JIS_ESC_START; return 0; }
        if (b == 0x5C) { d->jis_output_flag = 0; enc_put(o, 0x00A5); return 0; }   /* the yen sign */
        if (b == 0x7E) { d->jis_output_flag = 0; enc_put(o, 0x203E); return 0; }   /* the overline */
        if (b < 0x80 && b != 0x0E && b != 0x0F && b != 0x1B) {
            d->jis_output_flag = 0;
            enc_put(o, b);
            return 0;
        }
        d->jis_output_flag = 0;
        if (!fatal) enc_put(o, 0xFFFD);
        return -1;
    case JIS_KATAKANA:
        if (b == 0x1B) { d->jis_state = JIS_ESC_START; return 0; }
        if (b >= 0x21 && b <= 0x5F) { d->jis_output_flag = 0; enc_put(o, 0xFF61u - 0x21u + b); return 0; }
        d->jis_output_flag = 0;
        if (!fatal) enc_put(o, 0xFFFD);
        return -1;
    case JIS_LEAD:
        if (b == 0x1B) { d->jis_state = JIS_ESC_START; return 0; }
        if (b == 0x24 || (b >= 0x21 && b <= 0x7E)) {
            d->jis_output_flag = 0;
            d->jis_lead = b;
            d->jis_state = JIS_TRAIL;
            return 0;
        }
        d->jis_output_flag = 0;
        if (!fatal) enc_put(o, 0xFFFD);
        return -1;
    case JIS_TRAIL:
        if (b == 0x1B) {
            d->jis_state = JIS_ESC_START;
            if (!fatal) enc_put(o, 0xFFFD);
            return -1;
        }
        d->jis_state = JIS_LEAD;
        if (b >= 0x21 && b <= 0x7E) {
            pointer = ((uint32_t)(d->jis_lead - 0x21)) * 94u + (uint32_t)(b - 0x21);
            cp = ENC_INDEX_CP(ENCODING_JIS0208, ENCODING_JIS0208_N, pointer);
            if (cp) { enc_put(o, cp); return 0; }
            if (!fatal) enc_put(o, 0xFFFD);
            return -1;
        }
        if (!fatal) enc_put(o, 0xFFFD);
        return -1;
    case JIS_ESC_START:
        if (b == 0x24 || b == 0x28) { d->jis_lead = b; d->jis_state = JIS_ESC; return 0; }
        dec_push_back(d, b);
        d->jis_output_flag = 0;
        d->jis_state = d->jis_out;
        if (!fatal) enc_put(o, 0xFFFD);
        return -1;
    default: {
        uint8_t lead = d->jis_lead;
        uint8_t state = 0xFF;
        DCHECK(d->jis_state == JIS_ESC, "the ISO-2022-JP decoder ran in a state it does not have");
        d->jis_lead = 0;
        if (lead == 0x28 && b == 0x42) state = JIS_ASCII;
        else if (lead == 0x28 && b == 0x4A) state = JIS_ROMAN;
        else if (lead == 0x28 && b == 0x49) state = JIS_KATAKANA;
        else if (lead == 0x24 && (b == 0x40 || b == 0x42)) state = JIS_LEAD;
        if (state != 0xFF) {
            d->jis_state = d->jis_out = state;
            /* The standard's `output` flag: two escapes in a row with nothing between them is an error. */
            if (d->jis_output_flag) {
                d->jis_output_flag = 0;
                if (!fatal) enc_put(o, 0xFFFD);
                return -1;
            }
            d->jis_output_flag = 1;
            return 0;
        }
        dec_push_back(d, lead);
        dec_push_back(d, b);
        d->jis_output_flag = 0;
        d->jis_state = d->jis_out;
        if (!fatal) enc_put(o, 0xFFFD);
        return -1;
    }
    }
}

/* Run `len` bytes through the receiver's decoder. Returns -1 with a TypeError live in fatal mode. */
static int decoder_run(EncDecoder *d, const uint8_t *p, size_t len, EncBuf *o)
{
    int row = ENCODING_SINGLE_BYTE_ROW[d->enc];
    size_t i = 0;

    /* THE QUEUE COMES FIRST. A byte pushed back is read before the caller's next one, and a byte pushed back
       from a previous decode() call is read before this call's first — which is what makes a sequence split
       across two chunks recover the same way an unsplit one does. */
    while (i < len || d->nback > 0) {
        uint8_t b;
        int repeat = 0, err = 0;

        if (d->nback > 0) {
            b = d->back[0];
            d->nback--;
            memmove(d->back, d->back + 1, d->nback);
        } else {
            b = p[i++];
        }

        if (d->enc == ENC_UTF_8) {
            err = utf8_step(d, b, o, &repeat, d->fatal);
        } else if (d->enc == ENC_UTF_16LE || d->enc == ENC_UTF_16BE) {
            err = utf16_step(d, b, o, &repeat, d->fatal, d->enc == ENC_UTF_16BE);
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
        } else if (d->enc == ENC_GB18030 || d->enc == ENC_GBK) {
            /* §11.1.1 serves BOTH: gbk is gb18030 with an encoder difference, and the standard gives it no
               decoder of its own. */
            err = gb18030_step(d, b, o, d->fatal);
        } else if (d->enc == ENC_BIG5) {
            err = big5_step(d, b, o, d->fatal);
        } else if (d->enc == ENC_EUC_JP) {
            err = euc_jp_step(d, b, o, d->fatal);
        } else if (d->enc == ENC_SHIFT_JIS) {
            err = shift_jis_step(d, b, o, d->fatal);
        } else if (d->enc == ENC_EUC_KR) {
            err = euc_kr_step(d, b, o, d->fatal);
        } else if (d->enc == ENC_ISO_2022_JP) {
            err = iso2022jp_step(d, b, o, d->fatal);
        } else {
            DFAIL("a page asked to decode an encoding this engine has no decoder for — every one the standard "
                  "names now has one, so reaching this means a NEW encoding was added to the registry and the "
                  "decode was not");
        }
        /* §4.1's "fatal" error mode RETURNS the error to whoever is processing the queue; it does not itself
           know what a caller makes of one. §7.1's decode() makes it a TypeError, and §6's byte-sequence hooks
           below have no realm to throw into at all — reaching for a `ctx` here is the whole reason this
           function used to need one. */
        if (err && d->fatal) return -1;
        if (repeat) dec_push_back(d, b);
    }
    return 0;
}

enum { DEC_ENCODING = 0, DEC_FATAL, DEC_IGNORE_BOM };

static JSValue js_decoder_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    EncDecoder *d = JS_GetOpaque(this_val, g_dec_class);
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

/* RUN BYTES THROUGH A DECODER — §7.1's decode() minus its arguments, which is exactly what §7.5's "decode and
   enqueue a chunk" and "flush and enqueue" are. The `stream` flag decides whether an incomplete sequence at
   the end is HELD for the next call or flushed as an error, which is the whole reason a decoder has state at
   all; the streaming interface is the same decoder driven by a TransformStream rather than by a page's calls,
   so this is one operation with two callers and not two implementations that will drift. */
JSValue enc_decoder_decode(JSContext *ctx, EncDecoder *d, const uint8_t *p, size_t len, bool stream)
{
    JSValue r;
    EncBuf o = { 0 };

    DCHECK(d != NULL, "bytes were run through a decoder that does not exist");
    if (decoder_run(d, p, len, &o) < 0) {
        /* §7.1 step 4: "If … a decoder error occurs, then throw a TypeError." The decoder answered the error;
           this is the caller that has a realm to turn it into one. */
        free(o.b);
        decoder_reset(d);
        return JS_ThrowTypeError(ctx, "the encoded data was not valid %s", ENCODING_NAMES[d->enc]);
    }
    if (!stream) {
        /* THE FLUSH. An incomplete sequence held across the last chunk is an error here — a stream that ends
           mid-character is not a character, and holding it silently would drop bytes the page handed over. */
        /* §11.1.1 step 1 counts gb18030's three held bytes for the same reason: a four-byte sequence cut short
           by the end of the stream is not a character either. */
        /* Every decoder's held state counts: a sequence cut short by the end of the stream is not a character
           whichever machine was holding it. §13.3.1 adds one of its own — ISO-2022-JP is incomplete when it is
           mid-escape OR when it ends in a two-byte mode, because the mode itself is unfinished. */
        int incomplete = d->needed != 0 || d->half >= 0 || d->lead_surrogate >= 0 ||
                         d->gb_first != 0 || d->gb_second != 0 || d->gb_third != 0 || d->mb_lead != 0 ||
                         (d->enc == ENC_ISO_2022_JP &&
                          (d->jis_state == JIS_ESC_START || d->jis_state == JIS_ESC ||
                           d->jis_state == JIS_TRAIL));
        if (incomplete) {
            if (d->fatal) {
                free(o.b);
                decoder_reset(d);
                return JS_ThrowTypeError(ctx, "the encoded data ended mid-sequence");
            }
            /* §13.3.1's escape states PREPEND what they were holding before returning the error, and the
               standard says so for the EOF byte too — so `ESC $` at the end of a stream is U+FFFD followed by
               the `$` it was holding, not U+FFFD alone. The flush is a STEP of the decoder, which means what
               it pushes back is then decoded. */
            if (d->enc == ENC_ISO_2022_JP) {
                if (d->jis_state == JIS_ESC) dec_push_back(d, d->jis_lead);
                d->jis_lead = 0;
                d->jis_output_flag = 0;
                d->jis_state = d->jis_out;
            }
            enc_put(&o, 0xFFFD);
            if (d->nback) {
                /* This arm is the NON-fatal one — the fatal flush returned above — and "replacement" has no
                   error to hand back, so a failure here would mean the mode was not the one this branch is. */
                int r2 = decoder_run(d, NULL, 0, &o);
                DCHECK(r2 == 0, "the flush's pushback answered a decoder error in \"replacement\" error mode, "
                                "which pushes U+FFFD and continues rather than returning one");
                (void)r2;
            }
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
    return r;
}

/* §7.1's decode(): the arguments, then the operation above. */
static JSValue js_decoder_decode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    EncDecoder *d = JS_GetOpaque(this_val, g_dec_class);
    const uint8_t *p = NULL;
    size_t len = 0;
    JSValue buf = JS_UNDEFINED, r;
    bool stream;

    (void)magic;
    if (!d) return JS_ThrowTypeError(ctx, "not a TextDecoder");
    if (argc > 0 && !JS_IsUndefined(argv[0]) && enc_buffer_source(ctx, argv[0], &p, &len, &buf) < 0)
        return JS_EXCEPTION;
    stream = idl_dict_bool(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "stream");
    r = enc_decoder_decode(ctx, d, p, len, stream);
    JS_FreeValue(ctx, buf);
    return r;
}

/* ---- WHAT §7.5 AND §7.6 REACH THIS COMPONENT THROUGH ------------------------------------------------------
 *
 * The streaming interfaces are the same decoders and the same encoder, driven by a TransformStream rather than
 * by a page's calls. They live in their own component because the STREAM is the problem they solve, but the
 * codecs are this one's and are exported rather than written a second time. */

int encoding_lookup(const char *label, size_t len) { return encoding_get(label, len); }

bool encoding_is_replacement(int enc) { return enc == ENC_REPLACEMENT; }

const char *encoding_name_of(int enc)
{
    DCHECK(enc >= 0 && enc < ENC_COUNT, "the name of an encoding this registry does not have was asked for");
    return ENCODING_NAMES[enc];
}

EncDecoder *enc_decoder_new(int enc, bool fatal, bool ignore_bom)
{
    EncDecoder *d;
    DCHECK(enc >= 0 && enc < ENC_COUNT && enc != ENC_REPLACEMENT,
           "a decoder was asked for an encoding that has none — `replacement` is refused by the two "
           "constructors, and every other id in the registry decodes");
    d = calloc(1, sizeof *d);
    CHECK(d, "encoding: OOM building a decoder");
    d->enc = (EncodingId)enc;
    d->fatal = fatal;
    d->ignore_bom = ignore_bom;
    decoder_reset(d);
    return d;
}

void enc_decoder_free(EncDecoder *d) { free(d); }

bool enc_decoder_fatal(const EncDecoder *d) { return d->fatal != 0; }
bool enc_decoder_ignore_bom(const EncDecoder *d) { return d->ignore_bom != 0; }
int  enc_decoder_encoding(const EncDecoder *d) { return (int)d->enc; }

int encoding_buffer_source(JSContext *ctx, JSValueConst v, const uint8_t **pp, size_t *plen, JSValue *pbuf)
{
    return enc_buffer_source(ctx, v, pp, plen, pbuf);
}

/* ---- §6's HOOKS FOR STANDARDS, OVER BYTES -----------------------------------------------------------------
 *
 * A decoder INSTANCE is what the standard passes to "process a queue", and these two build one on the stack:
 * the hook runs a whole byte sequence to its end and keeps nothing between calls, which is the one shape the
 * heap-allocated streaming decoder exists for and this does not need. */

/* "To UTF-8 decode without BOM an I/O queue of bytes ioQueue given an optional I/O queue of scalar values
   output …: Process a queue with an instance of UTF-8's decoder, ioQueue, output, and "replacement". Return
   output." */
char *encoding_utf8_decode_without_bom(const char *p, size_t n, size_t *out_n)
{
    EncDecoder d;
    EncBuf o = { 0 };
    int r;

    memset(&d, 0, sizeof d);
    d.enc = ENC_UTF_8;
    d.fatal = 0;                       /* the hook's error mode is "replacement", which is what makes U+FFFD */
    decoder_reset(&d);
    r = decoder_run(&d, (const uint8_t *)p, n, &o);
    DCHECK(r == 0, "a decode in \"replacement\" error mode answered an error — that mode pushes U+FFFD and "
                   "continues, so there is nothing for it to answer with");
    (void)r;
    /* THE END OF THE QUEUE. "If byte is end-of-queue and UTF-8 bytes needed is not 0, then set UTF-8 bytes
       needed to 0 and return error" — a sequence the input stops in the middle of is one error, and
       "replacement" makes it one U+FFFD. Without this a trailing `%C3` would vanish instead. */
    if (d.needed != 0) enc_put(&o, 0xFFFD);
    /* NUL-TERMINATED BESIDE THE LENGTH, which is the contract every byte-string in this engine has: the length
       is the truth (a decoded U+0000 is a character), and the terminator is what lets a sequence without one
       be handed to a C string reader. */
    {
        char *g = realloc(o.b, o.n + 1);
        CHECK(g, "encoding: OOM terminating a UTF-8 decode");
        o.b = g;
        o.b[o.n] = 0;
    }
    if (out_n) *out_n = o.n;
    return o.b;
}

bool encoding_is_scalar_value_string(const char *p, size_t n)
{
    EncDecoder d;
    EncBuf o = { 0 };
    int r;

    memset(&d, 0, sizeof d);
    d.enc = ENC_UTF_8;
    /* "fatal" is the mode that RETURNS the error instead of replacing it, so it is the mode a PREDICATE asks
       in — and asking the decoder is what keeps this one statement about well-formedness rather than a second
       validator that would disagree with the decoder about an overlong form. */
    d.fatal = 1;
    decoder_reset(&d);
    r = decoder_run(&d, (const uint8_t *)p, n, &o);
    free(o.b);
    /* A sequence the bytes END inside is not a scalar value either — that is the same end-of-queue error the
       decode hook above turns into U+FFFD. */
    return r == 0 && d.needed == 0;
}

/* §7.1's constructor. `label` resolving to `replacement` is a RangeError as much as an unknown label is — the
   replacement encoding exists to make a hostile label decode to one error rather than to something scriptable,
   and the standard refuses to let a page name it. */
/* WHERE THIS MACHINE RESTS. §7.1's constructor is five steps and none of them can run the page's code — the
   declaration has converted `label` to a DOMString and the options dictionary to booleans before this is
   entered — so the machine has one stage and never returns to it. */
#define DEC_CTOR_STAGES(X) \
    X(DEC_CTOR_BUILD = IDL_STEP_FIRST, \
      "Encoding §7.1 new TextDecoder(label, options) steps 1-5 (get an encoding from label, the RangeError a " \
      "failure or `replacement` is, then this's encoding, error mode and ignore BOM)")
enum { DEC_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DEC_CTOR_STEPS[] = { DEC_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct { int unused; } JSDecoderCtorState;

static void js_dec_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

static int js_dec_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSValueConst options = argc > 1 ? argv[1] : JS_UNDEFINED;
    const char *label = "utf-8";
    size_t label_len = 5;
    int id;
    EncDecoder *d;
    JSValue obj;

    (void)st; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == DEC_CTOR_BUILD, "the TextDecoder constructor resumed at a stage §7.1 does not have");
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

    {
        JSValue proto = JS_GetClassProto(ctx, g_dec_class);
        DCHECK(!JS_IsNull(proto), "a TextDecoder was minted in a realm that never ran its install");
        obj = JS_NewObjectProtoClass(ctx, proto, g_dec_class);
        JS_FreeValue(ctx, proto);
    }
    if (JS_IsException(obj)) return -1;
    d = enc_decoder_new(id, idl_dict_bool(ctx, options, "fatal"),
                        idl_dict_bool(ctx, options, "ignoreBOM"));
    JS_SetOpaque(obj, d);
    *presult = obj;
    return 0;
}

static const IdlStepDecl js_dec_ctor_decl = {
    js_dec_ctor_step, sizeof(JSDecoderCtorState), js_dec_ctor_visit, NULL,
    "Encoding §7.1 new TextDecoder(label, options)", DEC_CTOR_STEPS
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
    /* forced-exec TIME-TRAVEL: THE BYTES BELOW ARE THE PAGE'S OWN BUFFER, so they are shared state and the
       running flow's delta must hold them before the first one changes. The engine routes every typed-array
       and DataView writer of its own through cow_capture_bytes, and nothing in it can see THIS write — a host
       component was handed a raw pointer, so `enc.encodeInto(s, sharedU8)` left one flow's bytes standing in
       the baseline for every sibling and standing through the unapply whose job is to take them back out. The
       unit is the BUFFER and not the view's elements for the reason cow.h gives, which is why the capture
       names `view` (JS_GetArrayBufferView answers with the buffer object, the same one the engine's hook
       passes). BEFORE the coercion below, because a `toString` that detaches this buffer must find its bytes
       already recorded. */
    cow_capture_buffer(ctx, view);
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

/* WHERE THIS MACHINE RESTS. §7.2's constructor steps "are to do nothing" — so the one stage this machine has
   is the object §3.7.1 creates, and there is nothing after it. */
#define ENC_CTOR_STAGES(X) \
    X(ENC_CTOR_BUILD = IDL_STEP_FIRST, \
      "Encoding §7.2 new TextEncoder() (the constructor steps are to do nothing; Web IDL §3.7.1's `new` " \
      "requirement and the object it creates are the whole of it)")
enum { ENC_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const ENC_CTOR_STEPS[] = { ENC_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct { int unused; } JSEncoderCtorState;

static void js_enc_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

static int js_enc_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    (void)st; (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == ENC_CTOR_BUILD, "the TextEncoder constructor resumed at a stage §7.2 does not have");
    if (JS_IsUndefined(hdr->this_val))
        return JS_ThrowTypeError(ctx, "constructor TextEncoder requires 'new'"), -1;
    {
        JSValue proto = JS_GetClassProto(ctx, g_enc_class);
        DCHECK(!JS_IsNull(proto), "a TextEncoder was minted in a realm that never ran its install");
        *presult = JS_NewObjectProtoClass(ctx, proto, g_enc_class);
        JS_FreeValue(ctx, proto);
    }
    return JS_IsException(*presult) ? -1 : 0;
}

static const IdlStepDecl js_enc_ctor_decl = {
    js_enc_ctor_step, sizeof(JSEncoderCtorState), js_enc_ctor_visit, NULL,
    "Encoding §7.2 new TextEncoder()", ENC_CTOR_STEPS
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

    g_id_decode = idl_method_id_dict(ctx, DECODE_ARGS, 2, DECODE_OPTIONS,
                                     (int)(sizeof DECODE_OPTIONS / sizeof DECODE_OPTIONS[0]),
                                     js_decoder_decode, 0);
    idl_optional_from(0);   /* §7.1: `decode(optional BufferSource input, optional TextDecodeOptions options)` */
    g_id_encode = idl_method_id(ctx, ENCODE_ARGS, 1, js_encoder_encode, 0);
    idl_optional_from(0);   /* §7.2: `encode(optional USVString input = "")` */
    g_id_encode_into = idl_method_id(ctx, ENCODE_INTO_ARGS, 2, js_encoder_encode_into, 0);

    g_dec_ctor_stepid = idl_method_id_step(ctx, DEC_CTOR_ARGS, 2, DECODER_OPTIONS,
                                           (int)(sizeof DECODER_OPTIONS / sizeof DECODER_OPTIONS[0]),
                                           &js_dec_ctor_decl, 0);
    idl_optional_from(0);   /* §7.1: `constructor(optional DOMString label = "utf-8", optional options = {})` */
    g_enc_ctor_stepid = idl_method_id_step(ctx, NULL, 0, NULL, 0, &js_enc_ctor_decl, 0);
    realm_declare_intrinsic(encoding_install_protos);
}

/* §7.1's AND §7.2's INTERFACE PROTOTYPE OBJECTS, FOR ONE REALM. */
void encoding_install_protos(JSContext *ctx)
{
    JSValue dec_p, enc_p, prev;

    DCHECK(g_dec_class != 0, "a realm asked for TextDecoder.prototype before the class was declared");
    prev = JS_GetClassProto(ctx, g_dec_class);
    DCHECK(JS_IsNull(prev), "encoding_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);

    dec_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(dec_p), "TextDecoder.prototype could not be allocated");
    idl_interface_tag(ctx, dec_p, "TextDecoder");
    idl_install_accessor(ctx, dec_p, "encoding", js_decoder_get, DEC_ENCODING, -1);
    idl_install_accessor(ctx, dec_p, "fatal", js_decoder_get, DEC_FATAL, -1);
    idl_install_accessor(ctx, dec_p, "ignoreBOM", js_decoder_get, DEC_IGNORE_BOM, -1);
    idl_install_method(ctx, dec_p, "decode", 0, g_id_decode);
    JS_SetClassProto(ctx, g_dec_class, dec_p);

    enc_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(enc_p), "TextEncoder.prototype could not be allocated");
    idl_interface_tag(ctx, enc_p, "TextEncoder");
    idl_install_accessor(ctx, enc_p, "encoding", js_encoder_get, 0, -1);
    idl_install_method(ctx, enc_p, "encode", 0, g_id_encode);
    idl_install_method(ctx, enc_p, "encodeInto", 2, g_id_encode_into);
    JS_SetClassProto(ctx, g_enc_class, enc_p);
}

void encoding_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;
    DCHECK(g_dec_ctor_stepid >= 0, "the Encoding globals were installed before encoding_init declared them");
    ctor = idl_step_constructor(ctx, "TextDecoder", 0, g_dec_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the TextDecoder interface object could not be allocated");
    {
        JSValue proto = JS_GetClassProto(ctx, g_dec_class);
        DCHECK(!JS_IsNull(proto), "TextDecoder was installed into a realm that never ran its proto build");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "TextDecoder", ctor);

    ctor = idl_step_constructor(ctx, "TextEncoder", 0, g_enc_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the TextEncoder interface object could not be allocated");
    {
        JSValue proto = JS_GetClassProto(ctx, g_enc_class);
        DCHECK(!JS_IsNull(proto), "TextEncoder was installed into a realm that never ran its proto build");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "TextEncoder", ctor);
}

void encoding_free(JSContext *ctx)
{
    if (!g_enc_rt)
        return;
    /* the prototypes are the REALMS' — released with their contexts */
    g_enc_rt = NULL;
    g_dec_ctor_stepid = g_enc_ctor_stepid = -1;
}
