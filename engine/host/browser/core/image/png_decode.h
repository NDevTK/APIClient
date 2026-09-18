/* PNG PIXELS — the Portable Network Graphics (PNG) Specification (Third Edition) read as the specification it
 * is, turning one PNG datastream into one non-premultiplied RGBA8 bitmap and nothing else. It knows about
 * chunks, filters, palettes and sample depths; it knows nothing about elements, realms, flows or the DOM.
 *
 * WHY THIS IS A SEPARATE COMPONENT FROM core/image/image_header.h, WHICH ALSO READS AN IHDR. That file
 * answers css-images-3 §4.1 "Object-Sizing Terminology"'s natural dimensions for EVERY image format, as a
 * pure allocation-free function of the bytes, reachable from a reply delivery that must run none of the
 * page's code — and its own banner says what it is deliberately not: "No pixel decode, no colour management,
 * no interlace pass reconstruction". It reaches PNG §11.2.1 "IHDR Image header"'s Width and Height at FIXED
 * OFFSETS, because PNG §11.2.1 says "The IHDR chunk shall be the first chunk in the PNG datastream" and that
 * fixes their position without a walk. This file walks PNG §5.3 "Chunk layout"'s chunks because it must — the
 * image data is in chunks whose position nothing fixes — and reads the WHOLE of PNG §11.2.1's seven fields,
 * five of which that struct deliberately does not carry.
 * SO THEY ARE NOT MERGED, AND THE THING THAT KEEPS THEM FROM DRIFTING IS AN ASSERTION RATHER THAN A SHARED
 * FUNCTION: whenever both answer, they must answer the SAME width and height, and the fixture asserts exactly
 * that over every vector. Merging them the other way — a decoder called from a header read — would put an
 * allocating decompressor on the path of a question HTML §4.8.4.3.5 "Updating the image data" asks about
 * every reply, which is the one property that file exists to have. Giving `ImageHeader` the other five fields
 * would be five members no consumer of a natural dimension reads, which is the defect CLAUDE.md counts seven
 * instances of.
 *
 * ── THE BYTES ARE A STRANGER'S, AND THAT DECIDES EVERY REFUSAL BELOW ─────────────────────────────────────
 * A PNG reaches this component from a server nobody here controls, so CLAUDE.md's rule applies whole: a value
 * this codebase did not compute is INPUT and never an invariant. Every malformed thing a datastream can be —
 * a signature that is not PNG §5.2's eight bytes, a PNG §5.3 length running past the body, a PNG §5.5 CRC that disagrees,
 * a PNG §5.6 ordering violation, an IHDR field combination PNG §11.2.1 Table 12 does not allow, a palette index
 * PNG §11.2.2 calls "an error", a scanline naming a sixth filter type where PNG §9.2 Table 11 defines five, a
 * datastream that does not decompress, an unknown CRITICAL chunk — is a REFUSAL carried in `PngStatus`, and
 * NOT ONE OF THEM IS A `DCHECK`. An assert on any of them hands every server on the internet an abort switch
 * for this engine. The asserts that ARE here are about what a CALLER passed in and about values this file
 * itself computed, which is this codebase's own logic and is exactly what a `DCHECK` is for.
 * ALLOCATION FAILURE IS A REFUSAL FOR THE REASON inflate.h GIVES AND FOR A SECOND ONE THIS FILE ADDS.
 * check.h files allocation under `CHECK`, and that is right wherever the size is one this codebase composed.
 * Here it is not: PNG §11.2.1's Width and Height are four-byte fields a stranger wrote, each admitting values up
 * to 2^31-1, so thirteen bytes of IHDR can ask for an allocation no address space has. Under `CHECK` that is
 * a remote abort switch with a thirteen-byte payload. `PNG_REFUSED_NO_ROOM` is the declared absence for it.
 *
 * AND THAT IS ALSO WHAT BOUNDS THE DECOMPRESSION, WITHOUT A CAP AND WITHOUT A BUDGET. inflate.h says in as
 * many words that it does not limit its output and that "Where the format's own OWNER states an expected
 * length — PNG does, since IHDR fixes a datastream's uncompressed size exactly — that is a SPEC check
 * belonging to the component that reads IHDR". This is that component. PNG §7.2 "Scanlines" fixes a scanline's
 * byte count from the width, the bit depth and the colour type; PNG §9.1 "Filter methods and filter types" says
 * filtering "transforms the byte sequence in a scanline to an equal length sequence of bytes preceded by the
 * filter type"; so the number of bytes PNG §10.2's zlib datastream decompresses to is an exact function of IHDR
 * and nothing else. A decode that produces more than that has not overrun a budget — it has contradicted its
 * own header, which is a statement about the bytes and is refused as one. That is why a PNG whose IDAT is a
 * decompression bomb stops at the size its own IHDR declared rather than at a number this file chose, and it
 * is CLAUDE.md §NO BOUNDS' distinction rather than a limit smuggled in: nothing here decides that work will
 * not happen, the format decides how much work there is.
 *
 * ── WHY A STEP MACHINE ───────────────────────────────────────────────────────────────────────────────────
 * CLAUDE.md §scheduler: every code flow at any depth must suspend and resume, never drive to completion. A
 * decode of a megapixel image is an unbounded stretch inside one C activation — nothing raises the yield bit,
 * nothing polls it, no sibling flow runs and no document interleaves for as long as it takes. inflate.h is
 * the settled precedent one file over and states the same reason; core/crypto/secure_hash.h states it again.
 * SO THIS DRIVES `inflate_step` AND NEVER `inflate_run`. A decoder that called the run loop would be exactly
 * the drive-to-completion caller that makes a step machine pointless, and it would hold the thread for the
 * whole decompression. `budget` is the number of bytes a step may MOVE — copied out of an IDAT, produced by
 * the decompressor, or written into the bitmap — before it answers `PNG_MORE`; the caller owns it, and zero
 * means the default. It is a granularity and not a bound: an offer to rest, declined, costs one predicate.
 *
 * ── WHAT THIS FILE DOES NOT DO, STATED AS RESIDUALS BECAUSE THE CODE IS RIGHT AND NARROWER ───────────────
 * NAMED RESIDUAL — PNG §8.1 "Interlace methods"' METHOD 1, Adam7. WHAT IS NOT COVERED: a datastream whose IHDR
 * interlace method is 1, which PNG §8.1 defines as seven reduced images whose pixels are selected by an 8-by-8
 * pattern. Method 0, "the null method", is decoded. WHAT THE NEXT DIFF BUILDS: the seven passes' origins and
 * strides as four tables of seven, a reduced image's width and height derived from them, and the scanline
 * reconstruction below run once per non-empty pass writing into the strided positions of the one bitmap —
 * PNG §9.2's own rule that "the entire prior scanline … shall be treated as being zeroes for the first scanline
 * of A REDUCED IMAGE" is already what the reconstruction below implements, so what is missing is the
 * geometry and not the filtering. HOW ITS ABSENCE WOULD SHOW: a page whose image is interlaced renders
 * nothing where a browser renders it, and a caller asking this component reads a refusal naming the
 * interlace method rather than a bitmap.
 * NAMED RESIDUAL — COLOUR MANAGEMENT. WHAT IS NOT COVERED: PNG §11.3.2's gAMA, cHRM, sRGB, iCCP and cICP, all of
 * which this file skips as the ancillary chunks they are, so a sample reaches the bitmap as the number the
 * datastream stated. WHAT THE NEXT DIFF BUILDS: PNG §13.13 "Decoder gamma handling"'s transfer applied between
 * the sample read and the bitmap write, with the chunk values carried on the header struct. HOW ITS ABSENCE
 * WOULD SHOW: an image encoded for a transfer curve other than the surface's renders at the wrong lightness,
 * observable as a screenshot whose midtones differ from the same page in a browser while its black and white
 * agree.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_IMAGE_PNG_DECODE_H
#define ENGINE_HOST_BROWSER_CORE_IMAGE_PNG_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/image/inflate.h"

/* WHAT A STEP ANSWERS. Two are states of a healthy decode and the rest are STATEMENTS ABOUT THE BYTES — a
   refusal never claims this engine went wrong, which is why each names what the datastream did. They are
   separate arms rather than one `PNG_REFUSED` because a caller reporting why a resource was rejected can only
   report what it was told, and because a fixture asserting the RIGHT refusal is the only thing that
   distinguishes a decoder that refuses correctly from one that refuses everything. */
typedef enum {
    PNG_DONE = 0,               /* PNG §11.2.4's IEND was reached and the image was reconstructed */
    PNG_MORE,                   /* the step's budget was spent; the state is intact, call again */
    PNG_REFUSED_SIGNATURE,      /* PNG §5.2 "PNG signature"'s eight bytes are not the first eight bytes */
    PNG_REFUSED_CHUNK_LAYOUT,   /* PNG §5.3 "Chunk layout": a length running past the body, a chunk cut short, or
                                   a length over PNG §7.1's 2^31-1 ceiling for a PNG four-byte unsigned integer */
    PNG_REFUSED_CRC,            /* PNG §5.5 "CRC algorithm"'s value disagrees with the chunk's own bytes */
    PNG_REFUSED_CHUNK_ORDER,    /* PNG §5.6 "Chunk ordering" Table 7's positions and multiplicities, or a chunk
                                   that section's lattice requires and the datastream does not carry */
    PNG_REFUSED_UNKNOWN_CRITICAL, /* PNG §13.1 "Error handling"'s third class: a chunk type this decoder does not
                                   know whose first byte has bit 5 clear. PNG §13.2 "Error checking": "An unknown
                                   chunk type is not to be treated as an error unless it is a critical
                                   chunk." It is a SEPARATE arm from the one above because the two take
                                   opposite readings — a misordered datastream is malformed, and this one is
                                   well formed and uses an extension this build has not been taught */
    PNG_REFUSED_IHDR,           /* PNG §11.2.1 "IHDR Image header": a zero dimension, a colour type / bit depth
                                   pair Table 12 does not list, or a compression, filter or interlace method
                                   this specification does not define */
    PNG_REFUSED_PALETTE,        /* PNG §11.2.2 "PLTE Palette": a length not divisible by three, more than 256
                                   entries, absent where the colour type requires it, present where it
                                   forbids it, or "any out-of-range pixel value found in the image data" */
    PNG_REFUSED_TRANSPARENCY,   /* PNG §11.3.1.1 "tRNS Transparency": a length that is not what the colour type
                                   states, or a tRNS chunk for colour types 4 and 6, for which that section
                                   says it "shall not appear" */
    PNG_REFUSED_FILTER_TYPE,    /* PNG §9.2 "Filter types for filter method 0" Table 11 defines five filter types
                                   and says the set "shall not be extended"; a scanline named a sixth */
    PNG_REFUSED_DATASTREAM,     /* PNG §10.2's zlib datastream did not decompress — `png_decode_datastream_status`
                                   carries which of inflate.h's refusals it was */
    PNG_REFUSED_IMAGE_LENGTH,   /* PNG §7.2 and PNG §9.1 fix the decompressed length exactly from PNG §11.2.1's fields and
                                   the datastream decompressed to a different one */
    PNG_REFUSED_INTERLACE,      /* PNG §8.1's method 1, Adam7 — THE ONE ARM THAT IS ABOUT THIS ENGINE RATHER THAN
                                   ABOUT THE BYTES. A method-1 datastream is well formed and this build does
                                   not reconstruct its passes; see the header's named residual. It is a
                                   refusal and not a `DFAIL` for the same reason every arm above is: the
                                   datastream came from a stranger, so crashing on it is an abort switch */
    PNG_REFUSED_NO_ROOM         /* the bitmap or the intermediate this datastream's own IHDR asks for is
                                   larger than this build can address or allocate — see the header */
} PngStatus;

/* PNG §11.2.1's SEVEN FIELDS, AS THE DATASTREAM STATED THEM. Held as the integers they are rather than as
   anything derived, so a reader can see what the file says; the derived quantities (channels per pixel, bytes
   per scanline, the filter distance) are this file's own arithmetic and are not published, because a second
   place free to compute them differently is a second place free to be wrong. */
typedef struct {
    uint32_t width;              /* PNG §11.2.1 Width, 4 bytes, never zero */
    uint32_t height;             /* PNG §11.2.1 Height, 4 bytes, never zero */
    uint8_t  bit_depth;          /* PNG §11.2.1 Bit depth: 1, 2, 4, 8 or 16, as Table 12 allows for the type */
    uint8_t  colour_type;        /* PNG §6.1 "Color types and values" Table 9: 0, 2, 3, 4 or 6 */
    uint8_t  compression_method; /* PNG §10.1 "Compression method 0" — only 0 is defined */
    uint8_t  filter_method;      /* PNG §9.1 — only 0 is defined */
    uint8_t  interlace_method;   /* PNG §8.1 — 0 (null) or 1 (Adam7) */
} PngHeader;

/* HOW MANY BYTES A STEP MOVES BEFORE IT OFFERS TO REST, when a caller states no preference. It is a DEFAULT
   and not a limit: a step that reaches it answers `PNG_MORE` and the next step continues from the same place.
   The number is inflate.h's own default, because in the phase that dominates a decode a step IS one
   `inflate_step` and a different number here would make the two components rest at different rates for no
   reason either of them could state. */
#define PNG_DECODE_DEFAULT_BUDGET INFLATE_DEFAULT_BUDGET

/* THE DECODER'S WHOLE STATE. A plain struct holding no pointer into anything but its own buffers and the
   caller's input, so a caller may park it between steps. */
typedef struct {
    /* THE DATASTREAM, BORROWED. The caller owns these bytes and must keep them alive for the decode's
       lifetime — asserted at every step, because a caller handing over a buffer it then frees is this
       codebase's own logic being wrong and is exactly what a DCHECK is for. */
    const uint8_t *in;
    size_t         in_len;
    size_t         in_pos;       /* the next byte the chunk walk will read */

    size_t         budget;       /* the policy input above, re-charged per step */
    int            phase;        /* this file's own vocabulary; see the .c. A caller has no business
                                    branching on it and a second consumer of a resume point is a second
                                    thing free to disagree about what a state means */

    PngHeader      hdr;
    bool           have_hdr;     /* PNG §11.2.1 was read — stated positively rather than left to be inferred
                                    from a zero width, which PNG §11.2.1 says is an invalid value anyway */

    /* PNG §5.6's ordering, as facts about what the walk has passed rather than as a state machine, because
       Table 7's constraints are pairwise and a machine would be a second statement of them. */
    bool           seen_plte;
    bool           seen_idat;
    bool           idat_closed;  /* PNG §11.2.3: multiple IDATs "shall appear consecutively with no other
                                    intervening chunks", so once a non-IDAT follows one, no more may come */
    bool           seen_iend;

    /* PNG §11.2.2's palette, at most 256 entries of three bytes, and PNG §11.3.1.1's alpha table over it. Fixed
       arrays because both sections bound them: "from 1 to 256 palette entries", and a tRNS for colour type 3
       "shall not contain more alpha values than there are palette entries". */
    uint8_t        plte[256 * 3];
    unsigned       plte_entries;
    uint8_t        plte_alpha[256];   /* PNG §11.3.1.1: entries a tRNS did not state "is assumed to be 255" */

    /* PNG §11.3.1.1's single transparent colour, for colour types 0 and 2. Held at the datastream's own sample
       precision because PNG §13.12 "Sample depth rescaling" says "transparent pixel detection shall be done
       before reducing sample precision" — a comparison after scaling would make two distinct 16-bit samples
       equal and turn opaque pixels transparent. */
    bool           have_trns_colour;
    uint16_t       trns_grey;
    uint16_t       trns_r, trns_g, trns_b;

    /* PNG §10.2's zlib datastream, assembled from the IDAT data fields. It is a COPY and it has to be: that
       section says "The concatenation of the contents of all the IDAT chunks makes up a zlib datastream" and
       the chunks are separated by their own length, type and CRC fields, so the datastream is not contiguous
       in the body. */
    uint8_t       *zdata;
    size_t         zdata_len;
    size_t         zdata_cap;

    /* THE CHUNK THE WALK IS STANDING IN. PNG §5.3's Length and Chunk Type, the running PNG §5.5 sum over it, and the
       five type questions, asked ONCE at the header and answered from these rather than re-compared at each
       resume — a type re-derived per step is a second reading of four bytes free to disagree with the first
       about which ordering rules were already applied. */
    uint32_t       chunk_len;
    size_t         chunk_pos;    /* bytes of the chunk's data field already summed and, for IDAT, copied */
    uint32_t       chunk_crc;
    bool           chunk_is_ihdr, chunk_is_plte, chunk_is_idat, chunk_is_trns, chunk_is_iend;

    Inflate        z;
    bool           z_live;       /* the decompressor has been initialised and owns memory */
    InflateStatus  z_status;     /* what it last answered — read back by `png_decode_datastream_status` */

    /* THE FILTERED SCANLINES, OWNED, RECONSTRUCTED IN PLACE. PNG §9.2's reconstruction writes Recon(x) over
       Filt(x) and every later byte reads the Recon values, so the reverse filter needs no second buffer —
       which is also why this is taken from the decompressor rather than copied out of it. */
    uint8_t       *raw;
    size_t         raw_len;
    size_t         raw_expect;   /* PNG §7.2 and PNG §9.1's exact length, computed from PNG §11.2.1's fields */
    size_t         row_bytes;    /* PNG §7.2's bytes in one scanline, not counting PNG §9.1's filter type byte */
    unsigned       filter_dist;  /* PNG §9.2 Table 10's distance from x to a: the bytes per pixel, or 1 "when the
                                    bit depth is less than 8" */
    uint32_t       row;          /* the scanline the reconstruction is standing at */

    /* THE ANSWER, OWNED: non-premultiplied RGBA8, width*height*4 bytes, top scanline first. */
    uint8_t       *rgba;
    size_t         rgba_len;

    /* PNG §12.4 "Sample depth scaling"'s linear equation, evaluated once per possible sample rather than per
       pixel, for the depths whose sample space fits in a byte. Depth 16 is computed directly. */
    uint8_t        scale8[256];
} PngDecode;

/* BEGIN A DECODE over a complete PNG datastream. `bytes` is BORROWED and must outlive the decode; `n` may be
   zero, which is an ordinary truncated datastream and not a hole. `budget` is the granularity a step rests
   at, and 0 means PNG_DECODE_DEFAULT_BUDGET.
   IT ALLOCATES NOTHING, so it cannot fail and has no status to return. */
void png_decode_init(PngDecode *d, const uint8_t *bytes, size_t n, size_t budget);

/* ADVANCE THE DECODE. Returns PNG_MORE when the step's budget is spent and the state is intact, PNG_DONE when
   PNG §11.2.4's IEND was reached and the bitmap is complete, and a refusal otherwise. Once a refusal or PNG_DONE
   is returned the state is FINISHED: calling again is this codebase misusing its own component, and is
   DCHECKed. */
PngStatus png_decode_step(PngDecode *d);

/* DRIVE IT TO AN ANSWER. Exactly `png_decode_step` in a loop and no second decoder — for a caller with
   nothing to interleave, such as a fixture or a trusted-zone tool. A caller that must park between turns
   calls `png_decode_step` and owns the loop itself; that is the whole difference and there is no other. */
PngStatus png_decode_run(PngDecode *d);

/* PNG §11.2.1's FIELDS, once the walk has read them. Answers false before that and says so positively, so a
   caller never reads a zero it cannot tell from an unset field. */
bool png_decode_header(const PngDecode *d, PngHeader *out);

/* THE BITMAP, while the decoder still owns it: non-premultiplied RGBA8, width*height*4 bytes, the top
   scanline first and within it the leftmost pixel first, which is PNG §7.2's own order. Valid only after
   PNG_DONE — a partial reconstruction is not a prefix of the answer the way a partial decompression is,
   because the bitmap is written scanline by scanline and the scanlines below the current one are whatever
   the allocation left there. `*n` states the length positively rather than leaving it to be recomputed. */
const uint8_t *png_decode_rgba(const PngDecode *d, size_t *n);

/* TAKE THE BITMAP. The caller becomes its owner and must `free` it; the decoder is left holding nothing, so
   `png_decode_free` after a take is correct and releases nothing twice. Returns NULL only when no bitmap was
   produced, which a zero `*n` states positively rather than leaving to be inferred from the pointer. */
uint8_t *png_decode_take_rgba(PngDecode *d, size_t *n);

/* WHICH OF inflate.h's REFUSALS PRODUCED A `PNG_REFUSED_DATASTREAM`, so that the nine ways PNG §10.2's zlib
   datastream can be malformed do not flatten into one. A caller that reports why a resource was rejected can
   only report what it was told, and "the image data did not decompress" is a strictly weaker report than "its
   ADLER32 disagreed". Answers INFLATE_DONE when no decompression has refused. */
InflateStatus png_decode_datastream_status(const PngDecode *d);

/* RELEASE whatever the decoder still owns. Idempotent, and safe on a decoder that refused. */
void png_decode_free(PngDecode *d);

/* PNG §5.5 "CRC algorithm"'s value over a byte sequence, exposed for the same reason inflate.h exposes Adler-32:
   a checksum with exactly one caller inside one file is a checksum nothing can test on published vectors.
   `crc` starts at 0 — the pre-conditioning PNG §5.5 describes ("the 32-bit CRC is initialized to all 1's") and
   the post-conditioning ("the CRC is inverted") are both INSIDE this function, so a caller sums a chunk in
   one call or in several and gets the same answer either way. */
uint32_t png_crc32(uint32_t crc, const uint8_t *bytes, size_t n);

#endif
