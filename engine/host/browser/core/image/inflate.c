/* See inflate.h — in particular its paragraph on why every malformed thing below is a REFUSAL and not an
   assert, and why an allocation failure here is one too. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/image/inflate.h"

/* WHERE THE DECODE IS STANDING. This file's own vocabulary; see the header's `phase` field for why no caller
   is given it. The order carries no meaning — a phase is a resume point and not a sequence number. */
enum {
    PH_WRAPPER_HEADER = 0,  /* RFC 1950 §2.2's CMF and FLG, read once and only under INFLATE_ZLIB */
    PH_BLOCK_HEADER,        /* RFC 1951 §3.2.3's BFINAL and BTYPE */
    PH_STORED,              /* RFC 1951 §3.2.4, mid-copy */
    PH_DYNAMIC_TABLES,      /* RFC 1951 §3.2.7's code trees, not yet read */
    PH_CODES,               /* RFC 1951 §3.2.5's symbol loop, between symbols */
    PH_COPY,                /* RFC 1951 §3.2.5's match copy, mid-copy */
    PH_WRAPPER_TRAILER,     /* RFC 1950 §2.2's ADLER32 */
    PH_FINISHED             /* a status has been returned and the state is spent */
};

/* ── RFC 1950 §2.2 "Data format"'s ADLER32 ──────────────────────────────────────────────────────────────── */

/* "s1 is the sum of all bytes, s2 is the sum of all s1 values. Both sums are done modulo 65521. s1 is
   initialized to 1, s2 to zero. The Adler-32 checksum is stored as s2*65536 + s1".
   THE MODULO IS TAKEN EVERY BYTE RATHER THAN DEFERRED, which is slower than the usual deferred form and is the
   right trade here: the deferred form is correct only while the accumulators cannot overflow before the next
   reduction, and that is a property of a BLOCK LENGTH the fast form must choose and this file would then have
   to state and defend. `s1` after a reduction is below 65521 and a byte is below 256, so neither sum can leave
   32 bits between reductions and the arithmetic below is total. */
uint32_t inflate_adler32(uint32_t s, const uint8_t *bytes, size_t n)
{
    uint32_t s1 = s & 0xFFFFu, s2 = (s >> 16) & 0xFFFFu;
    size_t i;

    DCHECK(bytes != NULL || n == 0, "inflate_adler32 was handed a NULL buffer with a nonzero length — that is "
                                    "a caller of this file passing a value this codebase composed, not a "
                                    "statement about anybody's bytes");
    for (i = 0; i < n; i++) {
        s1 = (s1 + bytes[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    return (s2 << 16) | s1;
}

/* ── RFC 1951 §3.1.1 "Packing into bytes"' BIT STREAM ───────────────────────────────────────────────────── */

/* "Data elements are packed into bytes in order of increasing bit number within the byte, i.e., starting with
   the least-significant bit of the byte" and "Data elements other than Huffman codes are packed starting with
   the least-significant bit of the data element".
   RETURNS FALSE WHEN THE INPUT IS SPENT, which is the one way this can fail and is a statement about the
   bytes. `need` is this file's own value at every call site and is asserted rather than refused. */
static bool bits_need(Inflate *z, unsigned need)
{
    DCHECK(need <= 24, "bits_need was asked for more bits than the 32-bit buffer can hold beside the up-to-7 "
                       "it may already carry — that is this file's own arithmetic, never the stream's");
    while (z->bitcnt < need) {
        if (z->in_pos >= z->in_len) return false;
        z->bitbuf |= (uint32_t)z->in[z->in_pos++] << z->bitcnt;
        z->bitcnt += 8;
    }
    return true;
}

/* TAKE `need` BITS, least-significant first. The caller has already established they are there. */
static uint32_t bits_take(Inflate *z, unsigned need)
{
    uint32_t v;

    DCHECK(z->bitcnt >= need, "bits_take was called without bits_need having answered true — this file's own "
                              "sequencing, and the one thing a bit reader cannot be asked to check for its "
                              "caller without becoming a second answer to whether the input is spent");
    v = z->bitbuf & ((1u << need) - 1u);
    z->bitbuf >>= need;
    z->bitcnt -= need;
    return v;
}

/* ── RFC 1951 §3.2.2 "Use of Huffman coding in the "deflate" format" ────────────────────────────────────── */

/* BUILD ONE ALPHABET FROM ITS CODE LENGTHS. That section's whole rule is that "we can define the Huffman code
   for an alphabet just by giving the bit lengths of the codes for each symbol of the alphabet in order", under
   its two constraints — "All codes of a given bit length have lexicographically consecutive values, in the
   same order as the symbols they represent" and "Shorter codes lexicographically precede longer codes". So a
   count per length plus the symbols ordered by (length, symbol) IS the code, and nothing else has to be
   stored.
   IT REFUSES AN OVER-SUBSCRIBED SET AND ADMITS AN INCOMPLETE ONE, and the asymmetry is the standard's own.
   RFC 1951 §3.2.7 states that "If only one distance code is used, it is encoded using one bit, not zero bits; in this
   case there is a single code length of one, with one unused code" — an INCOMPLETE code a conforming encoder
   really emits, so refusing it would refuse valid streams. An over-subscribed set describes no prefix code at
   all and is refused. An all-zero set is the empty alphabet RFC 1951 §3.2.7 also names ("One distance code of zero bits
   means that there are no distance codes used at all"), and is admitted; a symbol arriving for it is caught at
   the decode, where the refusal names the symbol rather than the table.
   THE LEFTOVER IS THIS FILE'S OWN ARITHMETIC AND IS ASSERTED, not the stream's: `left` starts at 1 and is
   doubled and decremented per length, so its going negative is over-subscription (a fact about the bytes,
   refused) while its ending outside the representable range would be this loop being wrong. */
static bool huff_build(uint16_t *count, uint16_t *symbol, size_t nsym, const uint8_t *lengths)
{
    unsigned l;
    size_t   s;
    int      left;
    uint16_t offs[16];

    DCHECK(nsym > 0 && nsym <= 288, "huff_build was asked for an alphabet outside the sizes RFC 1951 §3.2.5 "
                                    "and RFC 1951 §3.2.7 define — 288 literal/length, 32 distance, 19 code-length — "
                                    "which is this file's own call, never the stream's");
    for (l = 0; l < 16; l++) count[l] = 0;
    for (s = 0; s < nsym; s++) {
        DCHECK(lengths[s] < 16, "a code length above 15 reached huff_build — RFC 1951 §3.2.7 encodes a code length in "
                                "4 bits and the fixed alphabets state none longer, so this is a table this "
                                "file built wrongly and not a value a stream can state");
        count[lengths[s]]++;
    }
    /* A LENGTH OF ZERO IS NOT A CODE. RFC 1951 §3.2.7: "A code length of 0 indicates that the corresponding symbol …
       will not occur in the block, and should not participate in the Huffman code construction algorithm". */
    count[0] = 0;

    left = 1;
    for (l = 1; l < 16; l++) {
        left <<= 1;
        left -= (int)count[l];
        if (left < 0) return false;   /* over-subscribed: these lengths describe no prefix code */
    }

    offs[0] = 0;
    offs[1] = 0;
    for (l = 1; l < 15; l++) offs[l + 1] = (uint16_t)(offs[l] + count[l]);
    for (s = 0; s < nsym; s++) if (lengths[s] != 0) symbol[offs[lengths[s]]++] = (uint16_t)s;
    return true;
}

/* DECODE ONE SYMBOL. RFC 1951 §3.2.1 "Synopsis of prefix and Huffman coding": "A parser can decode the next symbol from
   an encoded input stream by walking down the tree from the root, at each step choosing the edge corresponding
   to the next input bit." With RFC 1951 §3.2.2's canonical ordering the walk needs no tree — at each length the codes
   of that length are consecutive and begin where the previous length's ended, so `code - first` is the index
   into that length's run of symbols.
   RFC 1951 §3.1.1: "Huffman codes are packed starting with the most-significant bit of the code", which is why the code
   is accumulated MSB-first out of a bit stream that is otherwise LSB-first.
   RETURNS -1 FOR A TRUNCATED INPUT AND -2 FOR BITS NO CODE DENOTES, because those are two different refusals
   and a caller that collapsed them would report a cut-off resource as a corrupt one. */
static int huff_decode(Inflate *z, const uint16_t *count, const uint16_t *symbol)
{
    unsigned l, code = 0, first = 0, index = 0;

    for (l = 1; l < 16; l++) {
        if (!bits_need(z, 1)) return -1;
        code |= (unsigned)bits_take(z, 1);
        if (code - first < count[l]) return (int)symbol[index + (code - first)];
        index += count[l];
        first = (first + count[l]) << 1;
        code <<= 1;
    }
    return -2;
}

/* ── RFC 1951 §3.2.5 "Compressed blocks (length and distance codes)"' TABLES ────────────────────────────── */

/* Transcribed from that section's two tables, in its own order. The length table is indexed by symbol-257 and
   the distance table by the distance code itself.
   CODES 286 AND 287 ARE NOT HERE AND THAT IS RFC 1951 §3.2.6's OWN STATEMENT — "Literal/length values 286-287 will
   never actually occur in the compressed data, but participate in the code construction" — so they are built
   into the fixed alphabet and REFUSED if one is ever decoded. Distance codes 30 and 31 are the same case. */
static const uint16_t LEN_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t LEN_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* RFC 1951 §3.2.7's code-length alphabet order: "in the order: 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2,
   14, 1, 15". */
static const uint8_t CLEN_ORDER[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* ── THE OUTPUT ─────────────────────────────────────────────────────────────────────────────────────────── */

/* GROW TO HOLD `extra` MORE BYTES. Returns false when it cannot, which the caller turns into
   INFLATE_REFUSED_NO_ROOM — see the header for why this is a refusal rather than a CHECK.
   THE OLD BUFFER SURVIVES A FAILED GROW, which matters because the caller may still hand its partial output
   to whoever asked: `realloc` returning NULL leaves the original allocation valid, so the result is taken
   through a temporary and only committed on success. */
static bool out_reserve(Inflate *z, size_t extra)
{
    size_t want;
    uint8_t *p;

    if (extra <= z->out_cap - z->out_len) return true;
    want = z->out_cap ? z->out_cap : 4096;
    while (want - z->out_len < extra) {
        size_t next = want * 2;
        if (next <= want) return false;   /* the size a stream is asking for does not fit a size_t */
        want = next;
    }
    p = realloc(z->out, want);
    if (p == NULL) return false;
    z->out = p;
    z->out_cap = want;
    return true;
}

/* EMIT ONE BYTE, keeping RFC 1950 §2.2's running Adler-32 with it. The checksum is folded in here rather than
   over the finished buffer so a decode is never walked twice, and so a caller that takes a partial output has
   a checksum consistent with what it took. */
static bool out_byte(Inflate *z, uint8_t b)
{
    if (!out_reserve(z, 1)) return false;
    z->out[z->out_len++] = b;
    z->adler_s1 = (z->adler_s1 + b) % 65521u;
    z->adler_s2 = (z->adler_s2 + z->adler_s1) % 65521u;
    return true;
}

/* ── THE ENTRIES ────────────────────────────────────────────────────────────────────────────────────────── */

void inflate_init(Inflate *z, const uint8_t *bytes, size_t n, InflateWrapping w, size_t budget)
{
    DCHECK(z != NULL, "inflate_init was handed no decoder to initialise");
    DCHECK(bytes != NULL || n == 0, "inflate_init was handed a NULL buffer with a nonzero length — a caller "
                                    "of this file passing a value this codebase composed");
    memset(z, 0, sizeof *z);
    z->in = bytes;
    z->in_len = n;
    z->wrapping = w;
    z->budget = budget ? budget : INFLATE_DEFAULT_BUDGET;
    z->phase = (w == INFLATE_ZLIB) ? PH_WRAPPER_HEADER : PH_BLOCK_HEADER;
    /* RFC 1950 §2.2: "s1 is initialized to 1, s2 to zero". */
    z->adler_s1 = 1;
    z->adler_s2 = 0;
}

/* RFC 1950 §2.2 "Data format"'s two header bytes, read whole because they are two bytes and a decoder that
   rested between them would be carrying a resume point for nothing. */
static InflateStatus wrapper_header(Inflate *z)
{
    unsigned cmf, flg;

    if (z->in_len - z->in_pos < 2) return INFLATE_REFUSED_TRUNCATED;
    cmf = z->in[z->in_pos];
    flg = z->in[z->in_pos + 1];
    z->in_pos += 2;

    /* "CM = 8 denotes the "deflate" compression method with a window size up to 32K. This is the method used
       by gzip and PNG". Any other CM is a stream this decompressor does not read. */
    if ((cmf & 0x0Fu) != 8u) return INFLATE_REFUSED_WRAPPER;
    /* "For CM = 8, CINFO is the base-2 logarithm of the LZ77 window size, minus eight … Values of CINFO above
       7 are not allowed in this version of the specification." */
    if ((cmf >> 4) > 7u) return INFLATE_REFUSED_WRAPPER;
    /* "The FCHECK value must be such that CMF and FLG, when viewed as a 16-bit unsigned integer stored in MSB
       order (CMF*256 + FLG), is a multiple of 31." */
    if (((cmf << 8) + flg) % 31u != 0u) return INFLATE_REFUSED_WRAPPER;
    /* "If FDICT is set, a DICT dictionary identifier is present immediately after the FLG byte." A preset
       dictionary is a stream whose first bytes were never transmitted, so nothing here can decode it — and
       PNG §10.1 "Compression method 0" forbids one outright, so refusing it costs this engine nothing it can
       reach. It is a REFUSAL rather than a DFAIL because the byte that sets it is a stranger's. */
    if (flg & 0x20u) return INFLATE_REFUSED_WRAPPER;

    z->phase = PH_BLOCK_HEADER;
    return INFLATE_MORE;
}

/* RFC 1950 §2.2's ADLER32, "stored as s2*65536 + s1 in most-significant-byte first (network) order". */
static InflateStatus wrapper_trailer(Inflate *z)
{
    uint32_t stated, got;

    /* "Any bits of input up to the next byte boundary are ignored" is RFC 1951 §3.2.4's rule for a stored block; the
       zlib trailer likewise begins at a byte boundary, the deflate data having ended wherever it ended. */
    z->bitbuf = 0;
    z->bitcnt = 0;
    if (z->in_len - z->in_pos < 4) return INFLATE_REFUSED_TRUNCATED;
    stated = ((uint32_t)z->in[z->in_pos] << 24) | ((uint32_t)z->in[z->in_pos + 1] << 16)
           | ((uint32_t)z->in[z->in_pos + 2] << 8) | (uint32_t)z->in[z->in_pos + 3];
    z->in_pos += 4;
    got = (z->adler_s2 << 16) | z->adler_s1;
    if (stated != got) return INFLATE_REFUSED_CHECKSUM;
    return INFLATE_DONE;
}

/* RFC 1951 §3.2.3 "Details of block format": "Each block of compressed data begins with 3 header bits …
   first bit BFINAL, next 2 bits BTYPE". */
static InflateStatus block_header(Inflate *z)
{
    unsigned btype;

    if (!bits_need(z, 3)) return INFLATE_REFUSED_TRUNCATED;
    z->final_block = bits_take(z, 1) != 0;
    btype = (unsigned)bits_take(z, 2);
    switch (btype) {
    case 0:
        /* RFC 1951 §3.2.4 "Non-compressed blocks (BTYPE=00)": "Any bits of input up to the next byte boundary are
           ignored. … LEN is the number of data bytes in the block. NLEN is the one's complement of LEN." */
        z->bitbuf = 0;
        z->bitcnt = 0;
        if (z->in_len - z->in_pos < 4) return INFLATE_REFUSED_TRUNCATED;
        {
            unsigned len  = (unsigned)z->in[z->in_pos] | ((unsigned)z->in[z->in_pos + 1] << 8);
            unsigned nlen = (unsigned)z->in[z->in_pos + 2] | ((unsigned)z->in[z->in_pos + 3] << 8);
            z->in_pos += 4;
            if ((len ^ 0xFFFFu) != nlen) return INFLATE_REFUSED_STORED_LENGTH;
            z->stored_left = len;
        }
        z->phase = PH_STORED;
        return INFLATE_MORE;
    case 1: {
        /* RFC 1951 §3.2.6 "Compression with fixed Huffman codes (BTYPE=01)"' two tables, stated as the code LENGTHS
           that section gives rather than as the codes it prints "for added clarity": 0-143 are 8 bits,
           144-255 are 9, 256-279 are 7 and 280-287 are 8; every distance code is a "(fixed-length) 5-bit"
           code, all 32 of them. */
        uint8_t lens[288];
        unsigned i;
        bool ok;
        for (i = 0;   i < 144; i++) lens[i] = 8;
        for (i = 144; i < 256; i++) lens[i] = 9;
        for (i = 256; i < 280; i++) lens[i] = 7;
        for (i = 280; i < 288; i++) lens[i] = 8;
        /* THE BUILD IS THE STATEMENT AND THE ASSERT IS ABOUT ITS RESULT, never the call. A `DCHECK` is
           compiled out in release, so a construction performed INSIDE one is a table that exists in the dev
           build and does not exist in the shipped one — the side-effect-free rule check.h states, and the one
           place in this file where breaking it would not fail until release. */
        ok = huff_build(z->lit_count, z->lit_symbol, 288, lens);
        DCHECK(ok, "RFC 1951 §3.2.6's FIXED literal/length lengths did not describe a prefix code — those lengths are "
                   "written above this line and are this file's own, so a failure here is this file being "
                   "wrong and is nothing any stream can cause");
        for (i = 0; i < 32; i++) lens[i] = 5;
        ok = huff_build(z->dist_count, z->dist_symbol, 32, lens);
        DCHECK(ok, "RFC 1951 §3.2.6's FIXED distance lengths did not describe a prefix code — the same, one table "
                   "over: thirty-two five-bit codes exactly fill a five-bit alphabet");
        (void)ok;
        z->phase = PH_CODES;
        return INFLATE_MORE;
    }
    case 2:
        z->phase = PH_DYNAMIC_TABLES;
        return INFLATE_MORE;
    default:
        /* RFC 1951 §3.2.3's own words for BTYPE 11: "reserved (error)". */
        return INFLATE_REFUSED_BLOCK_TYPE;
    }
}

/* RFC 1951 §3.2.7 "Compression with dynamic Huffman codes (BTYPE=10)"' code trees, read whole.
   IT IS NOT A REST POINT, and that is a statement about SIZE rather than a shortcut: the sequence it reads is
   at most HLIT+HDIST+258 = 320 code lengths, each at most 7 bits of code plus 7 of extra, so the whole of it
   is bounded by a few hundred bytes of input and produces NO output at all. A step boundary inside it would
   carry three more resume fields to rest inside work that cannot grow with the stream. */
static InflateStatus dynamic_tables(Inflate *z)
{
    unsigned hlit, hdist, hclen, i, n;
    uint8_t  clen[19];
    uint8_t  lens[288 + 32];
    uint16_t cl_count[16];
    uint16_t cl_symbol[19];

    /* "5 Bits: HLIT, # of Literal/Length codes - 257 (257 - 286)"; "5 Bits: HDIST, # of Distance codes - 1
       (1 - 32)"; "4 Bits: HCLEN, # of Code Length codes - 4 (4 - 19)". */
    if (!bits_need(z, 14)) return INFLATE_REFUSED_TRUNCATED;
    hlit  = (unsigned)bits_take(z, 5) + 257u;
    hdist = (unsigned)bits_take(z, 5) + 1u;
    hclen = (unsigned)bits_take(z, 4) + 4u;
    /* HLIT's five bits reach 288 where RFC 1951 §3.2.7 states 286, and a literal/length alphabet larger than the 288
       codes RFC 1951 §3.2.6 constructs is a set this format does not define. The distance count cannot exceed 32 and
       the code-length count cannot exceed 19 by arithmetic, so only this one needs refusing. */
    if (hlit > 288u) return INFLATE_REFUSED_CODE_LENGTHS;

    /* "(HCLEN + 4) x 3 bits: code lengths for the code length alphabet given just above, in the order: …" —
       and the orders this reads NOTHING for stay zero, which RFC 1951 §3.2.7 says means the symbol is not used. */
    for (i = 0; i < 19; i++) clen[i] = 0;
    for (i = 0; i < hclen; i++) {
        if (!bits_need(z, 3)) return INFLATE_REFUSED_TRUNCATED;
        clen[CLEN_ORDER[i]] = (uint8_t)bits_take(z, 3);
    }
    if (!huff_build(cl_count, cl_symbol, 19, clen)) return INFLATE_REFUSED_CODE_LENGTHS;

    /* "The code length repeat codes can cross from HLIT + 257 to the HDIST + 1 code lengths. In other words,
       all code lengths form a single sequence of HLIT + HDIST + 258 values." — so the two alphabets are read
       as ONE run and split afterwards, which is what makes a repeat crossing the boundary decode correctly. */
    n = 0;
    while (n < hlit + hdist) {
        int sym = huff_decode(z, cl_count, cl_symbol);
        if (sym == -1) return INFLATE_REFUSED_TRUNCATED;
        if (sym < 0)   return INFLATE_REFUSED_CODE_LENGTHS;
        if (sym < 16) {
            lens[n++] = (uint8_t)sym;
            continue;
        }
        {
            unsigned repeat, extra_bits;
            uint8_t  value;
            if (sym == 16) {
                /* "16: Copy the previous code length 3 - 6 times. The next 2 bits indicate repeat length
                   (0 = 3, … , 3 = 6)" — and there is no previous length at n == 0. */
                if (n == 0) return INFLATE_REFUSED_CODE_LENGTHS;
                value = lens[n - 1];
                extra_bits = 2;
                repeat = 3;
            } else if (sym == 17) {
                /* "17: Repeat a code length of 0 for 3 - 10 times. (3 bits of length)" */
                value = 0;
                extra_bits = 3;
                repeat = 3;
            } else {
                /* "18: Repeat a code length of 0 for 11 - 138 times (7 bits of length)" */
                DCHECK(sym == 18, "the code-length alphabet decoded a symbol outside 0..18 — the alphabet was "
                                  "built from 19 lengths by this file, so a symbol past its end is this "
                                  "file's own table being wrong and not anything the stream stated");
                value = 0;
                extra_bits = 7;
                repeat = 11;
            }
            if (!bits_need(z, extra_bits)) return INFLATE_REFUSED_TRUNCATED;
            repeat += (unsigned)bits_take(z, extra_bits);
            /* A repeat running past the single sequence RFC 1951 §3.2.7 describes is a stream stating more lengths
               than it declared, which is a fact about the bytes. */
            if (n + repeat > hlit + hdist) return INFLATE_REFUSED_CODE_LENGTHS;
            while (repeat-- > 0) lens[n++] = value;
        }
    }

    if (!huff_build(z->lit_count, z->lit_symbol, hlit, lens)) return INFLATE_REFUSED_CODE_LENGTHS;
    /* RFC 1951 §3.2.7: "One distance code of zero bits means that there are no distance codes used at all (the data is
       all literals)." That is an empty alphabet, which huff_build admits; a distance symbol arriving against
       it is refused at the decode, where the refusal can name what arrived. */
    if (!huff_build(z->dist_count, z->dist_symbol, hdist, lens + hlit)) return INFLATE_REFUSED_CODE_LENGTHS;
    return INFLATE_MORE;
}

/* RFC 1951 §3.2.4's copy, resumable: `stored_left` is what this block still owes. */
static InflateStatus stored_copy(Inflate *z, size_t *spend)
{
    while (z->stored_left > 0) {
        if (*spend == 0) return INFLATE_MORE;
        if (z->in_pos >= z->in_len) return INFLATE_REFUSED_TRUNCATED;
        if (!out_byte(z, z->in[z->in_pos++])) return INFLATE_REFUSED_NO_ROOM;
        z->stored_left--;
        (*spend)--;
    }
    z->phase = PH_BLOCK_HEADER;
    return INFLATE_MORE;
}

/* RFC 1951 §3.2.5's match copy, resumable: `copy_left` is what the match still owes and `copy_dist` is how far back it
   reads. THE COPY IS BYTE AT A TIME AND THAT IS THE STANDARD'S OWN REQUIREMENT, not an oversight — RFC 1951 §3.2.3:
   "the referenced string may overlap the current position; for example, if the last 2 bytes decoded have
   values X and Y, a string reference with <length = 5, distance = 2> adds X,Y,X,Y,X to the output stream."
   A block move would read the source as it was BEFORE the copy and produce X,Y and then whatever was there,
   so the overlap has to be resolved one byte at a time — which is also what makes resting mid-copy sound. */
static InflateStatus match_copy(Inflate *z, size_t *spend)
{
    while (z->copy_left > 0) {
        uint8_t b;
        if (*spend == 0) return INFLATE_MORE;
        DCHECK(z->copy_dist <= z->out_len, "a match's distance reaches past the start of the output, INSIDE "
                                           "the copy — the distance was checked against the output length "
                                           "when the match was read and the output only grows, so this is "
                                           "this file's own bookkeeping and not a claim about the stream");
        b = z->out[z->out_len - z->copy_dist];
        if (!out_byte(z, b)) return INFLATE_REFUSED_NO_ROOM;
        z->copy_left--;
        (*spend)--;
    }
    z->phase = PH_CODES;
    return INFLATE_MORE;
}

/* RFC 1951 §3.2.3's decoding algorithm for the actual data, entire:
     "loop (until end of block code recognized)
         decode literal/length value from input stream
         if value < 256 … copy value (literal byte) to output stream
         otherwise … if value = end of block (256) break from loop
            otherwise (value = 257..285) decode distance from input stream
               move backwards distance bytes in the output stream, and copy length bytes from this position"
   THE LOOP RESTS BETWEEN SYMBOLS, which needs no state at all beyond the bit position and the block's tables —
   both already on the decoder — and is why this is the natural rest point rather than a chosen one. */
static InflateStatus codes(Inflate *z, size_t *spend)
{
    while (*spend > 0) {
        int sym = huff_decode(z, z->lit_count, z->lit_symbol);
        if (sym == -1) return INFLATE_REFUSED_TRUNCATED;
        if (sym < 0)   return INFLATE_REFUSED_SYMBOL;

        if (sym < 256) {
            if (!out_byte(z, (uint8_t)sym)) return INFLATE_REFUSED_NO_ROOM;
            (*spend)--;
            continue;
        }
        if (sym == 256) {
            z->phase = PH_BLOCK_HEADER;
            return INFLATE_MORE;
        }
        /* RFC 1951 §3.2.5's length alphabet is 257..285. 286 and 287 "will never actually occur in the compressed
           data, but participate in the code construction", so a stream that does emit one is refused. */
        if (sym > 285) return INFLATE_REFUSED_DISTANCE;
        {
            unsigned li = (unsigned)sym - 257u;
            unsigned length, dist;
            int dsym;

            if (!bits_need(z, LEN_EXTRA[li])) return INFLATE_REFUSED_TRUNCATED;
            /* "The extra bits should be interpreted as a machine integer stored with the most-significant bit
               first" — which RFC 1951 §3.1.1 packs least-significant-bit-first into the stream, so reading them as an
               ordinary LSB-first field yields that integer. */
            length = LEN_BASE[li] + (unsigned)bits_take(z, LEN_EXTRA[li]);

            dsym = huff_decode(z, z->dist_count, z->dist_symbol);
            if (dsym == -1) return INFLATE_REFUSED_TRUNCATED;
            if (dsym < 0)   return INFLATE_REFUSED_SYMBOL;
            /* RFC 1951 §3.2.6: "distance codes 30-31 will never actually occur in the compressed data", and RFC 1951 §3.2.5's
               table defines none. */
            if (dsym > 29) return INFLATE_REFUSED_DISTANCE;
            if (!bits_need(z, DIST_EXTRA[dsym])) return INFLATE_REFUSED_TRUNCATED;
            dist = DIST_BASE[dsym] + (unsigned)bits_take(z, DIST_EXTRA[dsym]);

            /* RFC 1951 §3.2.3: "a distance cannot refer past the beginning of the output stream". This is the one
               check standing between a stranger's bytes and a read outside the buffer, so it is a REFUSAL
               about the stream and is made here, once, before any byte is copied. */
            if ((size_t)dist > z->out_len) return INFLATE_REFUSED_DISTANCE;

            z->copy_left = length;
            z->copy_dist = dist;
            z->phase = PH_COPY;
            return match_copy(z, spend);
        }
    }
    return INFLATE_MORE;
}

InflateStatus inflate_step(Inflate *z)
{
    size_t spend;

    DCHECK(z != NULL, "inflate_step was handed no decoder");
    DCHECK(z->phase != PH_FINISHED, "inflate_step was called on a decode that has already answered — a "
                                    "status is final, so a caller asking again is this codebase misusing "
                                    "its own component and never anything the bytes did");
    DCHECK(z->in != NULL || z->in_len == 0, "the input a decode borrowed is gone — the caller freed a buffer "
                                            "inflate_init was told would outlive the decode");
    spend = z->budget;

    for (;;) {
        InflateStatus st;
        switch (z->phase) {
        case PH_WRAPPER_HEADER:
            st = wrapper_header(z);
            break;
        case PH_BLOCK_HEADER:
            st = block_header(z);
            break;
        case PH_DYNAMIC_TABLES:
            st = dynamic_tables(z);
            if (st == INFLATE_MORE) z->phase = PH_CODES;
            break;
        case PH_STORED:
            st = stored_copy(z, &spend);
            break;
        case PH_CODES:
            st = codes(z, &spend);
            break;
        case PH_COPY:
            st = match_copy(z, &spend);
            break;
        default:
            DFAIL("inflate_step is standing in a phase this file does not define");
            return INFLATE_REFUSED_TRUNCATED;
        }

        if (st != INFLATE_MORE) {
            z->phase = PH_FINISHED;
            return st;
        }
        /* A BLOCK ENDED. RFC 1951 §3.2.3: "while not last block" — so BFINAL decides whether the next thing read is
           another block header or the end of the stream. */
        if (z->phase == PH_BLOCK_HEADER && z->final_block) {
            if (z->wrapping == INFLATE_ZLIB) {
                st = wrapper_trailer(z);
            } else {
                st = INFLATE_DONE;
            }
            z->phase = PH_FINISHED;
            return st;
        }
        if (spend == 0) return INFLATE_MORE;
    }
}

InflateStatus inflate_run(Inflate *z)
{
    for (;;) {
        InflateStatus st = inflate_step(z);
        if (st != INFLATE_MORE) return st;
    }
}

const uint8_t *inflate_bytes(const Inflate *z)
{
    DCHECK(z != NULL, "inflate_bytes was handed no decoder");
    return z->out;
}

size_t inflate_length(const Inflate *z)
{
    DCHECK(z != NULL, "inflate_length was handed no decoder");
    return z->out_len;
}

uint8_t *inflate_take(Inflate *z, size_t *n)
{
    uint8_t *p;

    DCHECK(z != NULL && n != NULL, "inflate_take was handed no decoder or nowhere to state the length");
    p = z->out;
    *n = z->out_len;
    z->out = NULL;
    z->out_len = 0;
    z->out_cap = 0;
    return p;
}

void inflate_free(Inflate *z)
{
    DCHECK(z != NULL, "inflate_free was handed no decoder");
    free(z->out);
    z->out = NULL;
    z->out_len = 0;
    z->out_cap = 0;
}
