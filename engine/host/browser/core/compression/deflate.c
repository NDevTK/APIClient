/* DEFLATE compression and its two framings. See deflate.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/compression/deflate.h"
#include "core/image/inflate.h"      /* inflate_adler32: RFC 1950 §2.2's ADLER32, one implementation */
#include "core/image/png_decode.h"   /* png_crc32: ISO 3309's CRC-32, which RFC 1952 §2.3.1 and PNG §5.5 share */
#include <lexbor/core/base.h>        /* lexbor_realloc: the allocator the engine gives its refusal edge */

/* RFC 1951 §3.2.4's LEN is two bytes, so one non-compressed block holds at most 65535 bytes. */
#define STORED_MAX 65535u

static void out_put(DeflateOut *o, const uint8_t *p, size_t n)
{
    if (o->n + n > o->cap) {
        size_t c = o->cap ? o->cap : 64;
        uint8_t *g;

        while (o->n + n > c) {
            CHECK(c * 2 > c, "deflate: an output length this engine composed does not fit a size_t");
            c *= 2;
        }
        g = lexbor_realloc(o->b, c);
        CHECK(g != NULL, "deflate: OOM growing compressed output");
        o->b = g;
        o->cap = c;
    }
    memcpy(o->b + o->n, p, n);
    o->n += n;
}

void deflate_init(Deflate *d, DeflateWrapping w)
{
    DCHECK(w == DEFLATE_RAW || w == DEFLATE_ZLIB || w == DEFLATE_GZIP,
           "deflate_init was handed a framing deflate.h does not define");
    memset(d, 0, sizeof *d);
    d->wrapping = w;
    /* RFC 1950 §2.2: "s1 is initialized to 1, s2 to zero"; png_crc32 starts at 0 (png_decode.h). */
    d->check = w == DEFLATE_ZLIB ? 1u : 0u;
}

static void header(Deflate *d, DeflateOut *out)
{
    if (d->header_written) return;
    d->header_written = true;
    if (d->wrapping == DEFLATE_ZLIB) {
        /* RFC 1950 §2.2: CM 8 with CINFO 7 (a 32K window), FLEVEL 0 ("compressor used fastest algorithm"),
           FDICT clear, and FCHECK making CMF*256 + FLG a multiple of 31: 0x7800 % 31 is 30, so FCHECK is 1. */
        static const uint8_t h[2] = { 0x78, 0x01 };
        _Static_assert(((0x78 << 8) | 0x01) % 31 == 0, "RFC 1950 §2.2's FCHECK makes CMF*256 + FLG a multiple of 31");
        out_put(out, h, sizeof h);
    } else if (d->wrapping == DEFLATE_GZIP) {
        /* RFC 1952 §2.3.1: ID1 31, ID2 139, CM 8, then FLG, MTIME and XFL at 0 and OS at 255, the defaults
           §2.3.1.2 Compliance gives "for all the other fields in the fixed-length part of the header". */
        static const uint8_t h[10] = { 0x1f, 0x8b, 0x08, 0, 0, 0, 0, 0, 0, 0xff };
        out_put(out, h, sizeof h);
    }
}

/* One RFC 1951 §3.2.3 block header with BTYPE 00 and §3.2.4's body. Every block this file writes starts on a
   byte boundary and is a whole number of bytes, so the three header bits and §3.2.4's padding are one byte. */
static void stored_block(DeflateOut *out, const uint8_t *p, size_t n, bool final)
{
    uint8_t h[5];

    DCHECK(n <= STORED_MAX, "a non-compressed block was asked to hold more bytes than RFC 1951 §3.2.4's LEN states");
    h[0] = final ? 1u : 0u;   /* BFINAL in bit 0, BTYPE 00 in bits 1-2 */
    h[1] = (uint8_t)(n & 0xFF);
    h[2] = (uint8_t)(n >> 8);
    h[3] = (uint8_t)(~n & 0xFF);   /* NLEN, "the one's complement of LEN" */
    h[4] = (uint8_t)((~n >> 8) & 0xFF);
    out_put(out, h, sizeof h);
    if (n) out_put(out, p, n);
}

void deflate_write(Deflate *d, const uint8_t *in, size_t n, DeflateOut *out)
{
    size_t at = 0;

    DCHECK(!d->finished, "deflate_write after deflate_finish — the caller wrote into an ended stream");
    DCHECK(in != NULL || n == 0, "deflate_write was handed a length with no bytes");
    if (n == 0) return;
    header(d, out);
    if (d->wrapping == DEFLATE_ZLIB) d->check = inflate_adler32(d->check, in, n);
    else if (d->wrapping == DEFLATE_GZIP) d->check = png_crc32(d->check, in, n);
    d->isize += (uint32_t)n;   /* RFC 1952 §2.3.1: "modulo 2^32", which unsigned arithmetic is */
    while (at < n) {
        size_t k = n - at < STORED_MAX ? n - at : STORED_MAX;

        stored_block(out, in + at, k, false);
        at += k;
    }
}

static void put_be32(DeflateOut *out, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    out_put(out, b, 4);
}

static void put_le32(DeflateOut *out, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    out_put(out, b, 4);
}

void deflate_finish(Deflate *d, DeflateOut *out)
{
    DCHECK(!d->finished, "deflate_finish ran twice over one stream");
    header(d, out);
    stored_block(out, NULL, 0, true);
    /* RFC 1950 §2.2 stores ADLER32 "in most-significant-byte first (network) order"; RFC 1952 §2.1 stores
       every multi-byte number least-significant byte first. */
    if (d->wrapping == DEFLATE_ZLIB) put_be32(out, d->check);
    else if (d->wrapping == DEFLATE_GZIP) { put_le32(out, d->check); put_le32(out, d->isize); }
    d->finished = true;
}
