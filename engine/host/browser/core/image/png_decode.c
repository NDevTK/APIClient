/* The Portable Network Graphics (PNG) Specification (Third Edition), decoded to non-premultiplied RGBA8.
 * See png_decode.h for why this is a step machine, why it is separate from core/image/image_header.h, and
 * why nothing a server sent is ever asserted here. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/image/inflate.h"
#include "core/image/png_decode.h"

/* WHERE THE DECODE IS STANDING. This enum is this file's own vocabulary rather than anything the standard
   names, which is why it lives here and not in the header: a caller has no business branching on it, and a
   second consumer of a resume point is a second thing free to disagree about what a state means. */
enum {
    PH_SIGNATURE = 0,   /* PNG §5.2's eight bytes */
    PH_CHUNK_HEADER,    /* PNG §5.3's Length and Chunk Type, and every ordering rule that reads only the type */
    PH_CHUNK_DATA,      /* PNG §5.3's Chunk Data, summed into PNG §5.5's CRC a budgeted slice at a time */
    PH_CHUNK_END,       /* PNG §5.3's CRC field, then the chunk's own meaning */
    PH_INFLATE,         /* PNG §10.2's zlib datastream, one inflate_step per step */
    PH_RECON,           /* PNG §9.2's reverse filter and the expansion to RGBA, one scanline per step */
    PH_FINISHED         /* PNG_DONE or a refusal was answered; a further step is a caller's own bug */
};

/* ---- PNG §7.1 "Integers and byte order" ------------------------------------------------------------------- */

/* "All integers that require more than one byte shall be in network byte order … the most significant byte
   comes first". The result is `uint32_t` because PNG §7.1 also says "PNG four-byte unsigned integers are limited
   to the range 0 to 2^31-1", so every value a conforming datastream can state is representable and a
   non-conforming one is still read without undefined behaviour — it is then REFUSED rather than trusted. */
static uint32_t png_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t png_be16(const uint8_t *p)
{
    return (uint16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]);
}

/* ---- PNG §5.5 "CRC algorithm" ----------------------------------------------------------------------------- */

/* "The CRC polynomial employed — which is identical to that used in the GZIP file format specification
   [RFC1952] — is x^32 + x^26 + x^23 + x^22 + x^16 + x^12 + x^11 + x^10 + x^8 + x^7 + x^5 + x^4 + x^2 + x + 1".
   THE CONSTANT BELOW IS THAT POLYNOMIAL REVERSED, and it is written as one number rather than as a shift per
   term because PNG §5.5 also fixes the bit order that makes the reversal correct: "the data from each byte is
   processed from the least significant bit (1) to the most significant bit (128) … the least significant bit
   of the 32-bit CRC is defined to be the coefficient of the x^31 term". With both ends reversed the update is
   a right shift, and the polynomial's coefficients land in the bits named above read from the other side:
   0xEDB88320 has bits 0,1,2,4,5,7,8,10,11,12,16,22,23,26 set, which is exactly the exponent list with the
   x^32 term implicit. That correspondence is asserted rather than trusted — see png_decode_selftest's
   published vector, and the 0xFFFFFFFF/0x2144DF1C pair that fails for a table built from the unreversed
   polynomial while a zero-length CRC still answers correctly. */
#define PNG_CRC_POLY 0xEDB88320u

static uint32_t png_crc_table[256];
static bool     png_crc_table_ready;

static void png_crc_build(void)
{
    uint32_t n, c, k;

    /* §D "Sample CRC implementation"'s construction, which is the same one RFC 1952 gives: each entry is the
       CRC of the one byte that indexes it, computed a bit at a time. */
    for (n = 0; n < 256; n++) {
        c = n;
        for (k = 0; k < 8; k++) c = (c & 1u) ? (PNG_CRC_POLY ^ (c >> 1)) : (c >> 1);
        png_crc_table[n] = c;
    }
    png_crc_table_ready = true;
}

uint32_t png_crc32(uint32_t crc, const uint8_t *bytes, size_t n)
{
    uint32_t c;
    size_t   i;

    DCHECK(bytes != NULL || n == 0,
           "png_crc32 was handed a byte count with no bytes — a length without its pointer is a caller of "
           "this file splitting a pair this codebase composed, which is this engine's own logic and not "
           "anything a datastream could state");
    if (!png_crc_table_ready) png_crc_build();

    /* PNG §5.5's PRE- AND POST-CONDITIONING, BOTH INSIDE, WHICH IS WHAT MAKES THIS COMPOSABLE. "the 32-bit CRC is
       initialized to all 1's … After all the data bytes are processed, the CRC is inverted". Inverting on the
       way in and on the way out means a caller that sums a chunk in one call and one that sums it in several
       reach the same answer, because the second call's inversion undoes the first's — which is the property
       the chunk walk below needs, since it sums a chunk a budgeted slice at a time. */
    c = ~crc;
    for (i = 0; i < n; i++) c = png_crc_table[(c ^ bytes[i]) & 0xFFu] ^ (c >> 8);
    return ~c;
}

/* ---- overflow-checked arithmetic over a stranger's dimensions ----------------------------------------- */

/* PNG §11.2.1's Width and Height are four-byte fields a stranger wrote, so every product of them is a number this
   file must be able to REFUSE rather than one it may compute and hope about. These return false when the
   result does not exist, and the caller's answer to that is PNG_REFUSED_NO_ROOM — never a `CHECK`, for the
   reason png_decode.h gives: the size was chosen by whoever wrote the datastream. */
static bool png_mul64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a != 0 && b > UINT64_MAX / a) return false;
    *out = a * b;
    return true;
}

static bool png_fits_size(uint64_t v, size_t *out)
{
    if (v > (uint64_t)SIZE_MAX) return false;
    *out = (size_t)v;
    return true;
}

/* ---- PNG §6.1 "Color types and values" and PNG §11.2.1 Table 12 ----------------------------------------------- */

/* PNG §4.5 "PNG image"'s sample counts, which PNG §6.1 Table 9's colour types name: "Truecolor with alpha: red,
   green, blue, alpha", "Greyscale with alpha: grey, alpha", "Truecolor: red, green, blue", "Greyscale: grey",
   "Indexed-color: palette index". Zero is returned for a colour type Table 9 does not list, which the caller
   reads as the refusal it is rather than as a channel count. */
static unsigned png_channels(unsigned colour_type)
{
    switch (colour_type) {
        case 0: return 1;
        case 2: return 3;
        case 3: return 1;
        case 4: return 2;
        case 6: return 4;
        default: return 0;
    }
}

/* PNG §11.2.1 Table 12 "Allowed combinations of color type and bit depth", transcribed row for row. The table is
   the whole of this function because the restriction is not derivable from anything else: PNG §11.2.1 says the
   combinations are restricted "to simplify implementations and to prohibit combinations that do not compress
   well", which is a choice the standard made and not a consequence of the packing. */
static bool png_depth_allowed(unsigned colour_type, unsigned bit_depth)
{
    switch (colour_type) {
        case 0: return bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8 || bit_depth == 16;
        case 2: return bit_depth == 8 || bit_depth == 16;
        case 3: return bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8;
        case 4: return bit_depth == 8 || bit_depth == 16;
        case 6: return bit_depth == 8 || bit_depth == 16;
        default: return false;
    }
}

/* ---- PNG §12.4 "Sample depth scaling" ---------------------------------------------------------------------- */

/* "output = floor((input * MAXOUTSAMPLE / MAXINSAMPLE) + 0.5)", with MAXOUTSAMPLE = 255 because the surface
   this feeds is core/graphics/raster_surface.h's RGBA8. PNG §12.4 calls this "The most accurate scaling method"
   for scaling UP and PNG §13.12 "Sample depth rescaling" calls it "The most accurate scaling" for scaling DOWN,
   so ONE equation serves both directions and there is no per-direction judgement to get wrong.
   THE INTEGER FORM IS EXACT RATHER THAN APPROXIMATE, and the reason is a property of the inputs: MAXINSAMPLE
   is 2^d - 1 and is therefore ODD for every d, so floor(x + 1/2) is (2*numerator + denominator) / (2*
   denominator) in integer division with no rounding of its own. The widest product is at depth 16 —
   2 * 65535 * 255 = 33423850 — which is why this is `uint32_t` and why that is not close to the edge. */
static uint8_t png_scale_to_8(unsigned bit_depth, uint32_t v)
{
    uint32_t maxin = (bit_depth >= 16) ? 65535u : ((1u << bit_depth) - 1u);
    return (uint8_t)((2u * v * 255u + maxin) / (2u * maxin));
}

/* ---- PNG §9.2 "Filter types for filter method 0" ----------------------------------------------------------- */

/* PNG §9.4 "Filter type 4: Paeth"'s PaethPredictor, transcribed from that section's own code:
     p = a + b - c;  pa = abs(p - a);  pb = abs(p - b);  pc = abs(p - c)
     if pa <= pb and pa <= pc then Pr = a  else if pb <= pc then Pr = b  else Pr = c
   "The calculations within the PaethPredictor function shall be performed exactly, without overflow" — hence
   `int`, in which a, b and c are bytes and p is at most 510 and at least -255. "The order in which the
   comparisons are performed is critical and shall not be altered", which is why the two `<=` tests are
   written in that order and neither is folded into a min. */
static unsigned png_paeth(unsigned a, unsigned b, unsigned c)
{
    int p  = (int)a + (int)b - (int)c;
    int pa = p > (int)a ? p - (int)a : (int)a - p;
    int pb = p > (int)b ? p - (int)b : (int)b - p;
    int pc = p > (int)c ? p - (int)c : (int)c - p;

    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* PNG §9.2 Table 11's Reconstruction Function column, run over one scanline in place. `line` holds Filt(x) on
   entry and Recon(x) on exit; `prev` is the ALREADY-RECONSTRUCTED previous scanline, or NULL for the first.
   "For all filters, the bytes 'to the left of' the first pixel in a scanline shall be treated as being zero.
   For filters that refer to the prior scanline, the entire prior scanline and bytes 'to the left of' the
   first pixel in the prior scanline shall be treated as being zeroes for the first scanline of a reduced
   image." — which is the whole of what the two conditionals below say.
   "Unsigned arithmetic modulo 256 is used, so that both the inputs and outputs fit into bytes", which is what
   the cast to `uint8_t` performs and is why nothing here clamps. */
static bool png_unfilter_row(unsigned ft, uint8_t *line, const uint8_t *prev, size_t n, unsigned dist)
{
    size_t i;

    switch (ft) {
        case 0:   /* None: Recon(x) = Filt(x) */
            return true;
        case 1:   /* Sub: Recon(x) = Filt(x) + Recon(a) */
            for (i = dist; i < n; i++) line[i] = (uint8_t)(line[i] + line[i - dist]);
            return true;
        case 2:   /* Up: Recon(x) = Filt(x) + Recon(b) */
            if (prev == NULL) return true;   /* b is zero for every byte, so the filter is the identity */
            for (i = 0; i < n; i++) line[i] = (uint8_t)(line[i] + prev[i]);
            return true;
        case 3:   /* Average: Recon(x) = Filt(x) + floor((Recon(a) + Recon(b)) / 2) */
            /* PNG §9.3 "Filter type 3: Average": "The sum Orig(a) + Orig(b) shall be performed without overflow
               (using at least nine-bit arithmetic)" — hence the `unsigned` sum before the shift, which is
               also PNG §9.3's own reading of floor(): "it is an integer division or right shift operation". */
            for (i = 0; i < n; i++) {
                unsigned a = (i >= dist) ? line[i - dist] : 0u;
                unsigned b = (prev != NULL) ? prev[i] : 0u;
                line[i] = (uint8_t)(line[i] + ((a + b) >> 1));
            }
            return true;
        case 4:   /* Paeth: Recon(x) = Filt(x) + PaethPredictor(Recon(a), Recon(b), Recon(c)) */
            for (i = 0; i < n; i++) {
                unsigned a = (i >= dist) ? line[i - dist] : 0u;
                unsigned b = (prev != NULL) ? prev[i] : 0u;
                unsigned c = (prev != NULL && i >= dist) ? prev[i - dist] : 0u;
                line[i] = (uint8_t)(line[i] + png_paeth(a, b, c));
            }
            return true;
        default:
            /* PNG §9.2: "Filter method 0 specifies exactly this set of five filter types and this shall not be
               extended." A sixth is a statement about the datastream and is refused by the caller. */
            return false;
    }
}

/* ---- PNG §7.2 "Scanlines" ---------------------------------------------------------------------------------- */

/* One sample out of a scanline, by its ordinal within that scanline. PNG §7.2: "Pixels within a scanline are
   always packed into a sequence of bytes with no wasted bits between pixels", "the leftmost sample in the
   high-order bits of a byte followed by the other samples for the scanline", and "PNG images that are not
   indexed-color images may have sample values with a bit depth of 16. Such sample values are in network byte
   order (MSB first, LSB second)." */
static uint32_t png_sample(const uint8_t *line, size_t index, unsigned bit_depth)
{
    unsigned per, shift, mask;

    if (bit_depth == 16) return (uint32_t)png_be16(line + index * 2u);
    if (bit_depth == 8)  return line[index];
    per   = 8u / bit_depth;
    shift = 8u - bit_depth * (unsigned)((index % per) + 1u);
    mask  = (1u << bit_depth) - 1u;
    return (line[index / per] >> shift) & mask;
}

/* ---- the decoder -------------------------------------------------------------------------------------- */

void png_decode_init(PngDecode *d, const uint8_t *bytes, size_t n, size_t budget)
{
    DCHECK(d != NULL, "png_decode_init was handed no decoder to initialise");
    DCHECK(bytes != NULL || n == 0,
           "png_decode_init was handed a NULL body with a nonzero length — a caller of this file passing a "
           "value this codebase composed");
    memset(d, 0, sizeof *d);
    d->in      = bytes;
    d->in_len  = n;
    d->budget  = budget ? budget : PNG_DECODE_DEFAULT_BUDGET;
    d->phase   = PH_SIGNATURE;
    d->z_status = INFLATE_DONE;
}

void png_decode_free(PngDecode *d)
{
    DCHECK(d != NULL, "png_decode_free was handed no decoder");
    if (d->z_live) { inflate_free(&d->z); d->z_live = false; }
    free(d->zdata); d->zdata = NULL; d->zdata_len = 0; d->zdata_cap = 0;
    free(d->raw);   d->raw   = NULL; d->raw_len   = 0;
    free(d->rgba);  d->rgba  = NULL; d->rgba_len  = 0;
}

bool png_decode_header(const PngDecode *d, PngHeader *out)
{
    DCHECK(d != NULL && out != NULL, "png_decode_header was handed no decoder or nowhere to put the answer");
    if (!d->have_hdr) return false;
    *out = d->hdr;
    return true;
}

const uint8_t *png_decode_rgba(const PngDecode *d, size_t *n)
{
    DCHECK(d != NULL && n != NULL, "png_decode_rgba was handed no decoder or nowhere to put the length");
    *n = d->rgba_len;
    return d->rgba;
}

uint8_t *png_decode_take_rgba(PngDecode *d, size_t *n)
{
    uint8_t *p;

    DCHECK(d != NULL && n != NULL, "png_decode_take_rgba was handed no decoder or nowhere to put the length");
    p = d->rgba;
    *n = d->rgba_len;
    d->rgba = NULL;
    d->rgba_len = 0;
    return p;
}

InflateStatus png_decode_datastream_status(const PngDecode *d)
{
    DCHECK(d != NULL, "png_decode_datastream_status was handed no decoder");
    return d->z_status;
}

/* A REFUSAL, RECORDED AND ANSWERED. Every refusal in this file goes through here so that the FINISHED phase
   and the returned status can never disagree — two places writing that pair is two places free to leave a
   decoder that has refused looking resumable. */
static PngStatus png_refuse(PngDecode *d, PngStatus st)
{
    d->phase = PH_FINISHED;
    return st;
}

/* GROW THE ASSEMBLED PNG §10.2 DATASTREAM. Its final size is not stated anywhere — PNG §11.2.3 allows any number of
   IDAT chunks of any length — but it has an exact upper bound that costs nothing to use: the data fields are
   a subset of the body, so the concatenation can never exceed `in_len`. Growing to that bound rather than
   doubling past it is not a cap in CLAUDE.md §NO BOUNDS' sense; it is the arithmetic fact that no more bytes
   than the caller handed over can arrive. */
static bool png_zdata_reserve(PngDecode *d, size_t need)
{
    size_t cap;
    uint8_t *p;

    if (need <= d->zdata_cap) return true;
    cap = d->zdata_cap ? d->zdata_cap * 2u : 4096u;
    if (cap < need) cap = need;
    if (cap > d->in_len && need <= d->in_len) cap = d->in_len;
    p = (uint8_t *)realloc(d->zdata, cap);
    if (p == NULL) return false;
    d->zdata = p;
    d->zdata_cap = cap;
    return true;
}

/* PNG §11.2.1's THIRTEEN BYTES, read and then checked against every constraint that section and PNG §8.1, PNG §9.1,
   PNG §10.1 and Table 12 place on them. Nothing here is asserted: each field is a number a stranger wrote. */
static PngStatus png_read_ihdr(PngDecode *d, const uint8_t *p, size_t len)
{
    unsigned ch;
    uint64_t row_bits, row_bytes, raw_expect, rgba;

    /* PNG §13.2 "Error checking": "For known-length chunks, such as IHDR, decoders should treat an unexpected
       chunk length as an error." */
    if (len != 13) return png_refuse(d, PNG_REFUSED_CHUNK_LAYOUT);

    d->hdr.width              = png_be32(p);
    d->hdr.height             = png_be32(p + 4);
    d->hdr.bit_depth          = p[8];
    d->hdr.colour_type        = p[9];
    d->hdr.compression_method = p[10];
    d->hdr.filter_method      = p[11];
    d->hdr.interlace_method   = p[12];

    /* PNG §11.2.1: "Width and height give the image dimensions in pixels. They are PNG four-byte unsigned
       integers. Zero is an invalid value." And PNG §7.1: "PNG four-byte unsigned integers are limited to the
       range 0 to 2^31-1", so a value with the top bit set is outside the type the field is declared as. */
    if (d->hdr.width == 0 || d->hdr.height == 0) return png_refuse(d, PNG_REFUSED_IHDR);
    if (d->hdr.width > 0x7FFFFFFFu || d->hdr.height > 0x7FFFFFFFu) return png_refuse(d, PNG_REFUSED_IHDR);

    /* PNG §11.2.1 Table 12, and the three method fields. PNG §13.2: "Unexpected values in fields of known chunks (for
       example, an unexpected compression method in the IHDR chunk) shall be checked for and treated as
       errors", and again: "Decoders shall check this byte and report an error if it holds an unrecognized
       code." PNG §9.1: "Only filter method 0 is defined by this specification." PNG §8.1: "Two interlace methods are
       defined in this International Standard, methods 0 and 1. Other values … are reserved". */
    if (!png_depth_allowed(d->hdr.colour_type, d->hdr.bit_depth)) return png_refuse(d, PNG_REFUSED_IHDR);
    if (d->hdr.compression_method != 0) return png_refuse(d, PNG_REFUSED_IHDR);
    if (d->hdr.filter_method != 0)      return png_refuse(d, PNG_REFUSED_IHDR);
    if (d->hdr.interlace_method > 1)    return png_refuse(d, PNG_REFUSED_IHDR);

    /* PNG §8.1's method 1, Adam7 — the one refusal in this file that is about THIS ENGINE. It is answered here,
       before any of the datastream's data is touched, because nothing below it would be reached anyway and a
       decoder that walked a whole body to refuse at the end would be spending a stranger's bytes to reach a
       conclusion its own header already settled. See png_decode.h's named residual. */
    if (d->hdr.interlace_method == 1) return png_refuse(d, PNG_REFUSED_INTERLACE);

    /* PNG §7.2's scanline and PNG §9.1's filter type byte, which together fix the decompressed length exactly:
       "Filtering transforms the byte sequence in a scanline to an equal length sequence of bytes preceded by
       the filter type." PNG §7.2: "Scanlines always begin on byte boundaries", hence the round up.
       EVERY PRODUCT IS OVERFLOW-CHECKED because both factors are a stranger's four-byte fields — see
       png_mul64's own comment. */
    ch = png_channels(d->hdr.colour_type);
    DCHECK(ch != 0,
           "a colour type reached the scanline arithmetic with no channel count — png_depth_allowed refuses "
           "every type PNG §6.1 Table 9 does not list, so this is this file's own two statements of that table "
           "disagreeing rather than anything a datastream could say");
    if (!png_mul64((uint64_t)d->hdr.width, (uint64_t)ch * d->hdr.bit_depth, &row_bits))
        return png_refuse(d, PNG_REFUSED_NO_ROOM);
    row_bytes = (row_bits + 7u) / 8u;
    if (!png_mul64((uint64_t)d->hdr.height, row_bytes + 1u, &raw_expect))
        return png_refuse(d, PNG_REFUSED_NO_ROOM);
    if (!png_mul64((uint64_t)d->hdr.width, (uint64_t)d->hdr.height, &rgba) || !png_mul64(rgba, 4u, &rgba))
        return png_refuse(d, PNG_REFUSED_NO_ROOM);
    if (!png_fits_size(row_bytes, &d->row_bytes)) return png_refuse(d, PNG_REFUSED_NO_ROOM);
    if (!png_fits_size(raw_expect, &d->raw_expect)) return png_refuse(d, PNG_REFUSED_NO_ROOM);
    if (!png_fits_size(rgba, &d->rgba_len)) return png_refuse(d, PNG_REFUSED_NO_ROOM);

    /* PNG §9.2 Table 10's distance from x to a: "the byte corresponding to x in the pixel immediately before the
       pixel containing x (or the byte immediately before x, when the bit depth is less than 8)". So it is the
       bytes per pixel, and exactly 1 wherever a pixel is narrower than a byte. */
    d->filter_dist = (ch * d->hdr.bit_depth) / 8u;
    if (d->filter_dist == 0) d->filter_dist = 1u;

    /* PNG §12.4's equation, evaluated once per sample value rather than once per sample, for the depths whose
       sample space fits in this table. Depth 16 goes through png_scale_to_8 directly. */
    if (d->hdr.bit_depth <= 8) {
        unsigned v, n = 1u << d->hdr.bit_depth;
        for (v = 0; v < n; v++) d->scale8[v] = png_scale_to_8(d->hdr.bit_depth, v);
    }

    /* PNG §11.3.1.1: entries a tRNS chunk does not state are opaque — "the alpha value for all remaining palette
       entries is assumed to be 255". Stated here rather than where a palette is read, because a datastream
       may carry a PLTE and no tRNS at all and the table must still be right. */
    memset(d->plte_alpha, 0xFF, sizeof d->plte_alpha);

    d->have_hdr = true;
    return PNG_MORE;
}

/* PNG §11.2.2 "PLTE Palette". */
static PngStatus png_read_plte(PngDecode *d, const uint8_t *p, size_t len)
{
    /* "The number of entries is determined from the chunk length. A chunk length not divisible by 3 is an
       error." and "The PLTE chunk contains from 1 to 256 palette entries". */
    if (len == 0 || (len % 3u) != 0 || len > 256u * 3u) return png_refuse(d, PNG_REFUSED_PALETTE);

    /* "it shall not appear for color types 0 and 4." */
    if (d->hdr.colour_type == 0 || d->hdr.colour_type == 4) return png_refuse(d, PNG_REFUSED_PALETTE);

    /* "The number of palette entries shall not exceed the range that can be represented in the image bit
       depth (for example, 2^4 = 16 for a bit depth of 4)." Only indexed-color images index the palette, so
       this is the constraint for colour type 3 and says nothing about a suggested palette for 2 or 6. */
    if (d->hdr.colour_type == 3 && (len / 3u) > (1u << d->hdr.bit_depth))
        return png_refuse(d, PNG_REFUSED_PALETTE);

    memcpy(d->plte, p, len);
    d->plte_entries = (unsigned)(len / 3u);
    return PNG_MORE;
}

/* PNG §11.3.1.1 "tRNS Transparency". */
static PngStatus png_read_trns(PngDecode *d, const uint8_t *p, size_t len)
{
    unsigned mask;

    /* "A tRNS chunk shall not appear for color types 4 and 6, since a full alpha channel is already present
       in those cases." */
    if (d->hdr.colour_type == 4 || d->hdr.colour_type == 6) return png_refuse(d, PNG_REFUSED_TRANSPARENCY);

    /* "For color types 0 or 2, two bytes per sample are used regardless of the image bit depth" — one sample
       for colour type 0 and three for colour type 2 — and "If the image bit depth is less than 16, the least
       significant bits are used. … decoders must mask the other bits to 0 before the value is used." */
    mask = (d->hdr.bit_depth >= 16) ? 0xFFFFu : ((1u << d->hdr.bit_depth) - 1u);
    if (d->hdr.colour_type == 0) {
        if (len != 2) return png_refuse(d, PNG_REFUSED_TRANSPARENCY);
        d->trns_grey = (uint16_t)(png_be16(p) & mask);
        d->have_trns_colour = true;
        return PNG_MORE;
    }
    if (d->hdr.colour_type == 2) {
        if (len != 6) return png_refuse(d, PNG_REFUSED_TRANSPARENCY);
        d->trns_r = (uint16_t)(png_be16(p)     & mask);
        d->trns_g = (uint16_t)(png_be16(p + 2) & mask);
        d->trns_b = (uint16_t)(png_be16(p + 4) & mask);
        d->have_trns_colour = true;
        return PNG_MORE;
    }

    /* Colour type 3: "The tRNS chunk shall not contain more alpha values than there are palette entries, but
       a tRNS chunk may contain fewer values than there are palette entries." The table was filled with 255 at
       IHDR time, which is that section's own rule for the entries this chunk does not reach. */
    if (len > d->plte_entries) return png_refuse(d, PNG_REFUSED_TRANSPARENCY);
    memcpy(d->plte_alpha, p, len);
    return PNG_MORE;
}

/* ONE RECONSTRUCTED SCANLINE BECOMES ONE ROW OF RGBA. PNG §6.2 "Alpha representation": "The color values in a
   pixel are not premultiplied by the alpha value assigned to the pixel", which is the same representation
   core/graphics/raster_surface.h holds and HTML §4.12.5.7 "Premultiplied alpha and the 2D rendering context"
   names — so the two agree by what each standard says and nothing here converts between them. */
static PngStatus png_expand_row(PngDecode *d, const uint8_t *line, uint8_t *dst)
{
    unsigned bd = d->hdr.bit_depth;
    uint32_t x;

    for (x = 0; x < d->hdr.width; x++) {
        uint8_t *o = dst + (size_t)x * 4u;

        switch (d->hdr.colour_type) {
            case 0: {   /* PNG §6.1 Table 9: Greyscale. One sample, which PNG §4.5 says "represents overall luminance". */
                uint32_t s = png_sample(line, x, bd);
                uint8_t  v = (bd <= 8) ? d->scale8[s] : png_scale_to_8(bd, s);
                /* PNG §13.12: "transparent pixel detection shall be done before reducing sample precision", so
                   the comparison is against the sample as the datastream stated it and never against `v`. */
                o[0] = o[1] = o[2] = v;
                o[3] = (d->have_trns_colour && s == d->trns_grey) ? 0u : 255u;
                break;
            }
            case 2: {   /* Truecolor: PNG §4.5's "triplet of samples: red, green, blue". */
                uint32_t r = png_sample(line, (size_t)x * 3u,      bd);
                uint32_t g = png_sample(line, (size_t)x * 3u + 1u, bd);
                uint32_t b = png_sample(line, (size_t)x * 3u + 2u, bd);
                o[0] = (bd <= 8) ? d->scale8[r] : png_scale_to_8(bd, r);
                o[1] = (bd <= 8) ? d->scale8[g] : png_scale_to_8(bd, g);
                o[2] = (bd <= 8) ? d->scale8[b] : png_scale_to_8(bd, b);
                o[3] = (d->have_trns_colour && r == d->trns_r && g == d->trns_g && b == d->trns_b) ? 0u : 255u;
                break;
            }
            case 3: {   /* Indexed-color: PNG §4.5's "index into a palette (and into an associated table of alpha
                           values, if present)". PNG §11.2.2: "any out-of-range pixel value found in the image
                           data is an error" — a statement about the datastream, refused as one.
                           NOTHING IS SCALED HERE: PNG §11.2.2 says "the palette uses 8 bits (1 byte) per sample
                           regardless of the image bit depth", so the bit depth describes the INDEX and the
                           entry it reaches is already at the surface's own precision. */
                uint32_t i = png_sample(line, x, bd);
                if (i >= d->plte_entries) return png_refuse(d, PNG_REFUSED_PALETTE);
                o[0] = d->plte[i * 3u];
                o[1] = d->plte[i * 3u + 1u];
                o[2] = d->plte[i * 3u + 2u];
                o[3] = d->plte_alpha[i];
                break;
            }
            case 4: {   /* Greyscale with alpha: PNG §4.5's "grey sample and an alpha sample", in that order. */
                uint32_t s = png_sample(line, (size_t)x * 2u,      bd);
                uint32_t a = png_sample(line, (size_t)x * 2u + 1u, bd);
                uint8_t  v = (bd <= 8) ? d->scale8[s] : png_scale_to_8(bd, s);
                o[0] = o[1] = o[2] = v;
                o[3] = (bd <= 8) ? d->scale8[a] : png_scale_to_8(bd, a);
                break;
            }
            default: {  /* 6 — Truecolor with alpha: PNG §4.5's "red, green, blue, alpha". png_depth_allowed has
                           already refused every colour type PNG §6.1 Table 9 does not list, so this arm is the
                           last member of a closed set rather than a catch-all. */
                uint32_t r = png_sample(line, (size_t)x * 4u,      bd);
                uint32_t g = png_sample(line, (size_t)x * 4u + 1u, bd);
                uint32_t b = png_sample(line, (size_t)x * 4u + 2u, bd);
                uint32_t a = png_sample(line, (size_t)x * 4u + 3u, bd);
                DCHECK(d->hdr.colour_type == 6,
                       "the RGBA expansion reached its last arm with a colour type PNG §6.1 Table 9 does not "
                       "list — png_depth_allowed refuses those, so this is this file's own two statements "
                       "of that table disagreeing rather than anything a datastream could say");
                o[0] = (bd <= 8) ? d->scale8[r] : png_scale_to_8(bd, r);
                o[1] = (bd <= 8) ? d->scale8[g] : png_scale_to_8(bd, g);
                o[2] = (bd <= 8) ? d->scale8[b] : png_scale_to_8(bd, b);
                o[3] = (bd <= 8) ? d->scale8[a] : png_scale_to_8(bd, a);
                break;
            }
        }
    }
    return PNG_MORE;
}

PngStatus png_decode_step(PngDecode *d)
{
    DCHECK(d != NULL, "png_decode_step was handed no decoder");
    DCHECK(d->in != NULL || d->in_len == 0,
           "a PNG decode stepped with a byte count and no bytes — the datastream is BORROWED and the caller "
           "must keep it alive for the decode's lifetime, so this is a caller that freed it underneath");
    DCHECK(d->phase != PH_FINISHED,
           "a PNG decode was stepped after it had answered PNG_DONE or a refusal — the state is finished at "
           "that point and stepping it again is this codebase misusing its own component");

    switch (d->phase) {

    case PH_SIGNATURE: {
        /* PNG §5.2 "PNG signature": "The first eight bytes of a PNG datastream always contain the following
           hexadecimal values: 89 50 4E 47 0D 0A 1A 0A". PNG §13.1: "Decoders should verify that all eight bytes
           of the PNG signature are correct." */
        static const uint8_t SIG[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
        if (d->in_len < 8 || memcmp(d->in, SIG, 8) != 0) return png_refuse(d, PNG_REFUSED_SIGNATURE);
        d->in_pos = 8;
        d->phase = PH_CHUNK_HEADER;
        return PNG_MORE;
    }

    case PH_CHUNK_HEADER: {
        uint32_t len;
        const uint8_t *type;
        bool is_ihdr, is_plte, is_idat, is_iend, is_trns, known;

        /* PNG §5.3: the Length and Chunk Type fields, eight bytes, ahead of the data. */
        if (d->in_len - d->in_pos < 8) return png_refuse(d, PNG_REFUSED_CHUNK_LAYOUT);
        len  = png_be32(d->in + d->in_pos);
        type = d->in + d->in_pos + 4;

        /* PNG §5.3: "Although encoders and decoders should treat the length as unsigned, its value shall not
           exceed 2^31-1 bytes." */
        if (len > 0x7FFFFFFFu) return png_refuse(d, PNG_REFUSED_CHUNK_LAYOUT);

        /* The whole chunk — data and PNG §5.3's four-byte CRC — must be in the body. A datastream that ends
           inside a chunk is truncated, which is PNG §13.1's "unexpected termination". */
        if ((uint64_t)d->in_len - d->in_pos < (uint64_t)8u + len + 4u)
            return png_refuse(d, PNG_REFUSED_CHUNK_LAYOUT);

        is_ihdr = memcmp(type, "IHDR", 4) == 0;
        is_plte = memcmp(type, "PLTE", 4) == 0;
        is_idat = memcmp(type, "IDAT", 4) == 0;
        is_iend = memcmp(type, "IEND", 4) == 0;
        is_trns = memcmp(type, "tRNS", 4) == 0;
        known   = is_ihdr || is_plte || is_idat || is_iend || is_trns;

        /* PNG §5.6 Table 7's positions, every one of which reads only the chunk TYPE and is therefore answered
           before a byte of the chunk's data is touched. PNG §11.2.1: "The IHDR chunk shall be the first chunk in
           the PNG datastream", and Table 7's "Multiple allowed: No" for it. */
        if (!d->have_hdr && !is_ihdr) return png_refuse(d, PNG_REFUSED_CHUNK_ORDER);
        if (d->have_hdr && is_ihdr)   return png_refuse(d, PNG_REFUSED_CHUNK_ORDER);

        /* PNG §11.2.3: "There may be multiple IDAT chunks; if so, they shall appear consecutively with no other
           intervening chunks." */
        if (is_idat && d->idat_closed) return png_refuse(d, PNG_REFUSED_CHUNK_ORDER);
        if (!is_idat && d->seen_idat)  d->idat_closed = true;

        /* Table 7: PLTE "Before first IDAT" and "Multiple allowed: No"; tRNS "After PLTE; before IDAT". */
        if (is_plte && (d->seen_idat || d->seen_plte)) return png_refuse(d, PNG_REFUSED_CHUNK_ORDER);
        if (is_trns && d->seen_idat) return png_refuse(d, PNG_REFUSED_CHUNK_ORDER);
        if (is_trns && d->hdr.colour_type == 3 && !d->seen_plte)
            return png_refuse(d, PNG_REFUSED_CHUNK_ORDER);

        if (is_idat) {
            /* PNG §13.1 names this as a syntax error in as many words: "the absence of a PLTE chunk before the
               first IDAT chunk in an indexed image". PNG §11.2.2: "For color type 3 (indexed-color), the PLTE
               chunk is required." */
            if (d->hdr.colour_type == 3 && !d->seen_plte) return png_refuse(d, PNG_REFUSED_PALETTE);
            d->seen_idat = true;
        }

        if (!known) {
            size_t i;
            /* PNG §13.2: "The chunk type can be checked for plausibility by seeing whether all four bytes are in
               the range codes 41-5A and 61-7A (hexadecimal); note that this need be done only for
               unrecognized chunk types." A type outside that range means the walk is out of step with the
               datastream rather than that an extension arrived, so it is a layout refusal. */
            for (i = 0; i < 4; i++) {
                uint8_t c = type[i];
                if (!((c >= 0x41u && c <= 0x5Au) || (c >= 0x61u && c <= 0x7Au)))
                    return png_refuse(d, PNG_REFUSED_CHUNK_LAYOUT);
            }
            /* PNG §13.1's three classes, and PNG §13.2's rule over them: "An unknown chunk type is not to be treated
               as an error unless it is a critical chunk." PNG §13.1: "unknown critical chunks (bit 5 of the first
               byte of the chunk type is 0)". */
            if ((type[0] & 0x20u) == 0u) return png_refuse(d, PNG_REFUSED_UNKNOWN_CRITICAL);
        }

        /* PNG §5.3: the CRC is "calculated on the preceding bytes in the chunk, including the chunk type field
           and chunk data fields, but not including the length field", so the running sum opens over the type
           and the data loop below continues it. */
        d->chunk_len = len;
        d->chunk_pos = 0;
        d->chunk_crc = png_crc32(0, type, 4);
        d->chunk_is_idat = is_idat;
        d->chunk_is_ihdr = is_ihdr;
        d->chunk_is_plte = is_plte;
        d->chunk_is_iend = is_iend;
        d->chunk_is_trns = is_trns;
        d->in_pos += 8;
        d->phase = PH_CHUNK_DATA;
        return PNG_MORE;
    }

    case PH_CHUNK_DATA: {
        size_t take = d->chunk_len - d->chunk_pos;

        if (take > d->budget) take = d->budget;
        if (take > 0) {
            const uint8_t *src = d->in + d->in_pos + d->chunk_pos;
            d->chunk_crc = png_crc32(d->chunk_crc, src, take);
            if (d->chunk_is_idat) {
                /* PNG §10.2: "The concatenation of the contents of all the IDAT chunks makes up a zlib
                   datastream", and "the boundaries between IDAT chunks are arbitrary and can fall anywhere
                   in the zlib datastream" — so the data fields are copied out and joined, and nothing here
                   may assume a chunk boundary is a boundary of anything the compressor knows about. */
                if (!png_zdata_reserve(d, d->zdata_len + take)) return png_refuse(d, PNG_REFUSED_NO_ROOM);
                memcpy(d->zdata + d->zdata_len, src, take);
                d->zdata_len += take;
            }
            d->chunk_pos += take;
        }
        if (d->chunk_pos == d->chunk_len) d->phase = PH_CHUNK_END;
        return PNG_MORE;
    }

    case PH_CHUNK_END: {
        const uint8_t *data = d->in + d->in_pos;
        uint32_t stored = png_be32(d->in + d->in_pos + d->chunk_len);
        PngStatus st;

        /* PNG §5.5, and PNG §13.1's "Detect errors as early as possible using the PNG signature bytes and CRCs on
           each chunk. … A CRC should be checked before processing the chunk data."
           IT IS CHECKED ON EVERY CHUNK RATHER THAN ON CRITICAL ONES ALONE, and that is a decision with a
           reason rather than a default. PNG §13.2 offers a lenient reading for ancillary chunks — "An unexpected
           value in an ancillary chunk can be handled by ignoring the whole chunk as though it were an unknown
           chunk type" — but it qualifies it in the same breath: "(This recommendation assumes that the
           chunk's CRC has been verified. In decoders that do not check CRCs, it is safer to treat any
           unexpected value as indicating a corrupted datastream.)" The lenient reading is about a chunk whose
           CRC AGREED and whose FIELD VALUE surprised, which is a different thing from a chunk whose CRC
           disagreed; and PNG §13.2's own warning about the consequence — "dropped/added data bytes or an
           erroneous chunk length can cause the decoder to get out of step and misinterpret subsequent data as
           a chunk header" — is about the walk rather than about the chunk. One uniform rule is also one fewer
           tier of judgement to state twice. */
        if (stored != d->chunk_crc) return png_refuse(d, PNG_REFUSED_CRC);

        d->in_pos += d->chunk_len + 4u;

        if (d->chunk_is_ihdr) {
            st = png_read_ihdr(d, data, d->chunk_len);
            if (st != PNG_MORE) return st;
        } else if (d->chunk_is_plte) {
            st = png_read_plte(d, data, d->chunk_len);
            if (st != PNG_MORE) return st;
            d->seen_plte = true;
        } else if (d->chunk_is_trns) {
            st = png_read_trns(d, data, d->chunk_len);
            if (st != PNG_MORE) return st;
        } else if (d->chunk_is_iend) {
            /* PNG §11.2.4: "The chunk's data field is empty." PNG §13.2's known-length rule again. */
            if (d->chunk_len != 0) return png_refuse(d, PNG_REFUSED_CHUNK_LAYOUT);
            /* PNG §5.6 Table 7 requires IDAT ("Multiple allowed: Yes" under the critical chunks that "shall
               appear in this order"), and PNG §5.1 "PNG datastream" makes the image the thing those chunks
               carry, so a datastream that reaches IEND having carried none is missing a chunk rather than
               describing an empty image. */
            if (!d->seen_idat) return png_refuse(d, PNG_REFUSED_CHUNK_ORDER);
            d->seen_iend = true;
            d->phase = PH_INFLATE;
            /* ANYTHING AFTER IEND IS NOT PART OF THIS DATASTREAM AND IS NOT REFUSED. PNG §5.2: the signature
               "indicates that the remainder of the datastream contains a single PNG image, consisting of a
               series of chunks beginning with an IHDR chunk and ending with an IEND chunk" — so the image
               ended here and what a transport appended after it is not a property of the image. */
            return PNG_MORE;
        }
        /* Every other chunk type is an ancillary one this decoder does not read. PNG §13.1's first class rule:
           a known chunk it chooses to treat as unknown, and PNG §13.2's "An unknown chunk type is not to be
           treated as an error unless it is a critical chunk" — its CRC was checked above and its bytes are
           skipped. */
        d->phase = PH_CHUNK_HEADER;
        return PNG_MORE;
    }

    case PH_INFLATE: {
        InflateStatus zs;

        if (!d->z_live) {
            /* PNG §10.1 "Compression method 0": "Deflate-compressed datastreams within PNG are stored in the
               zlib format", whose framing inflate.h names INFLATE_ZLIB. The step budget is handed down so
               that one png_decode_step is one inflate_step and the two components rest at one rate. */
            inflate_init(&d->z, d->zdata, d->zdata_len, INFLATE_ZLIB, d->budget);
            d->z_live = true;
        }
        zs = inflate_step(&d->z);
        d->z_status = zs;
        if (zs == INFLATE_MORE) {
            /* THE ANTI-BOMB PROPERTY, AND IT IS A SPEC CHECK RATHER THAN A CAP. PNG §7.2 and PNG §9.1 fix the
               decompressed length exactly from PNG §11.2.1's fields, so a datastream producing more than that
               has contradicted its own header. Asking after every step is what keeps the intermediate
               bounded by the image the header declared rather than by how much a stranger felt like
               compressing — inflate.h states that this check is this component's to make. */
            if (inflate_length(&d->z) > d->raw_expect) return png_refuse(d, PNG_REFUSED_IMAGE_LENGTH);
            return PNG_MORE;
        }
        if (zs != INFLATE_DONE) return png_refuse(d, PNG_REFUSED_DATASTREAM);

        d->raw = inflate_take(&d->z, &d->raw_len);
        inflate_free(&d->z);
        d->z_live = false;
        free(d->zdata);
        d->zdata = NULL;
        d->zdata_len = d->zdata_cap = 0;
        if (d->raw_len != d->raw_expect) return png_refuse(d, PNG_REFUSED_IMAGE_LENGTH);

        d->rgba = (uint8_t *)malloc(d->rgba_len ? d->rgba_len : 1u);
        if (d->rgba == NULL) return png_refuse(d, PNG_REFUSED_NO_ROOM);
        d->row = 0;
        d->phase = PH_RECON;
        return PNG_MORE;
    }

    case PH_RECON: {
        /* ONE SCANLINE PER STEP, AND THE UNIT IS PNG §7.2's OWN. CLAUDE.md §NO BOUNDS says to take the unit the
           spec names rather than the finest one the substrate can express: PNG §7.2 makes a scanline the thing a
           PNG image is a sequence of, PNG §9.1 gives each one exactly one filter type byte, and PNG §9.2's
           reconstruction reads the whole previous scanline — so a rest point inside a row would carry a
           column cursor for no gain, while the phase that dominates a decode already rests at the budget. */
        size_t stride = d->row_bytes + 1u;
        uint8_t *line = d->raw + (size_t)d->row * stride;
        const uint8_t *prev = (d->row == 0) ? NULL : (d->raw + ((size_t)d->row - 1u) * stride + 1u);
        unsigned ft = line[0];
        PngStatus st;

        /* PNG §9.2: "Filter method 0 specifies exactly this set of five filter types and this shall not be
           extended." A scanline naming a sixth is a statement about the datastream. */
        if (!png_unfilter_row(ft, line + 1u, prev, d->row_bytes, d->filter_dist))
            return png_refuse(d, PNG_REFUSED_FILTER_TYPE);

        st = png_expand_row(d, line + 1u, d->rgba + (size_t)d->row * d->hdr.width * 4u);
        if (st != PNG_MORE) return st;

        d->row++;
        if (d->row < d->hdr.height) return PNG_MORE;

        /* THE ONE ASSERT OVER THE FINISHED BITMAP, AND IT IS ABOUT THIS FILE'S OWN ARITHMETIC. Every number
           in it was computed here from PNG §11.2.1's fields; none of it is anything the datastream said. */
        DCHECK((uint64_t)d->row * d->hdr.width * 4u == (uint64_t)d->rgba_len,
               "a PNG reconstruction finished having written a number of bytes that is not the bitmap's own "
               "length — the row count, the width and the allocation are three statements of one product "
               "this file made, so a disagreement is this file's arithmetic and not a datastream's claim");
        d->phase = PH_FINISHED;
        return PNG_DONE;
    }

    default:
        DFAIL("a PNG decode stepped from a phase this file does not define — the phase is written only by "
              "this file and only to the members of its own enumeration");
        return png_refuse(d, PNG_REFUSED_CHUNK_LAYOUT);
    }
}

PngStatus png_decode_run(PngDecode *d)
{
    PngStatus st;

    DCHECK(d != NULL, "png_decode_run was handed no decoder");
    do { st = png_decode_step(d); } while (st == PNG_MORE);
    return st;
}
