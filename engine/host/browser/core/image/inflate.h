/* DEFLATE DECOMPRESSION — RFC 1951 "DEFLATE Compressed Data Format Specification version 1.3" read as the
 * specification it is, and RFC 1950 "ZLIB Compressed Data Format Specification version 3.3"'s framing over it.
 * This is a DECOMPRESSOR and nothing else: it turns one byte sequence into another and knows nothing about
 * images, chunks, realms, flows or the DOM.
 *
 * WHY THIS EXISTS AT ALL, AND WHY IT IS NOT A BIND. CLAUDE.md's ladder is host-runtime, then engine intrinsic,
 * then an existing Lexbor module, then a FAITHFUL SPEC PORT, and only then a hand-roll. The first three were
 * asked and all three answered no, by command rather than by recollection:
 *   - a word-boundary grep for `inflate`, `zlib`, `deflate`, `puff` and `z_stream` over every `.c` and `.h`
 *     under `engine` returns no implementation anywhere in this tree. Every hit is a DIFFERENT SENSE of the word (a solver
 *     comment about an INFLATED count), a GZIP media-type string in core/mime, or prose about emscripten's
 *     ports directory.
 *   - `git grep -rln -i 'inflate\|zlib\|deflate' -- engine/lexbor` answers nothing. Lexbor has no compressor.
 *   - The JS engine has none either; the same grep covers engine/qjs, which is ordinary tracked content.
 * WHAT WAS REFUSED, AND WHY IT IS A REFUSAL RATHER THAN AN OVERSIGHT. Emscripten really does carry a zlib
 * PORT, and the fourth rung is only reached once a bind has been declined on its merits, so the merits are
 * recorded here and not left to be re-derived. `tools/ports/zlib.py` in the installed toolchain FETCHES
 * `https://github.com/madler/zlib/archive/refs/tags/v1.3.2.tar.gz` when `-sUSE_ZLIB=1` is first set; the
 * emscripten sysroot in this checkout carries no zlib header at all. So the wasm host would take its
 * decompressor from a tarball downloaded AT BUILD TIME and recorded in no revision of this repository, while
 * the native host would take a DIFFERENT build of a DIFFERENT version from the machine's own libz. CLAUDE.md
 * §Testing's rule for a gate's inputs is that the REVISION must contain them — a number produced by a program
 * assembled partly from an untracked download is a number belonging to no revision, which is the one property
 * a frozen snapshot exists to guarantee. Two hosts taking one capability from two untracked places is that
 * defect twice. A port against a fetched RFC has no such input: it is text in this tree, at this revision, the
 * same program on both hosts.
 * AND IT IS NOT A HAND-ROLL, WHICH IS THE RUNG CLAUDE.md RESERVES FOR `never crypto/parsers from scratch`.
 * Every table, every field order and every algorithm below is transcribed from the RFC named beside it, and
 * the fixture checks the result against vectors this component did not produce. What the ban is about is
 * improvising a format from its shape; what this is, is reading the document.
 *
 * ── THE BYTES ARE A STRANGER'S. THIS IS THE LOAD-BEARING PARAGRAPH OF THE FILE. ──────────────────────────
 * A compressed stream reaches this component from a server nobody here controls. CLAUDE.md's rule for a value
 * this codebase did not compute is that it is INPUT and never an invariant, so EVERY malformed thing a stream
 * can be — a reserved block type, a stored block whose NLEN is not LEN's complement, an over-subscribed code
 * length set, a distance reaching back further than the output is long, a symbol no code denotes, a truncated
 * tail, a wrapper byte pair that is not a multiple of 31, a checksum that disagrees — is a REFUSAL carried in
 * `InflateStatus`, and NOT ONE OF THEM IS A `DCHECK`. An assert on any of them hands every server on the
 * internet an abort switch for this engine, which is the same defect core/image/image_header.h refuses one
 * layer up and the same one CLAUDE.md records a `google.rpc.Status` reply causing in the trusted zone.
 * The asserts that ARE here are about what a CALLER passed in and about values this file itself computed —
 * those are this codebase's own logic and are exactly what a `DCHECK` is for.
 *
 * AND ALLOCATION FAILURE IS A REFUSAL HERE, WHICH DEPARTS FROM check.h's RULE OF THUMB ON PURPOSE. check.h
 * files allocation under `CHECK` — always fatal, dev and release — and the reason it gives is sound wherever
 * it applies: "a dropped flow corrupts the frontier", so a size this codebase composed running out is a state
 * production must not proceed past. THE SIZE HERE IS NOT ONE THIS CODEBASE COMPOSED. DEFLATE states no output
 * length anywhere in its format, so how much memory a decode wants is chosen by whoever wrote the stream, and
 * a few kilobytes of input can ask for gigabytes. Under `CHECK` that is not an out-of-memory condition, it is
 * a REMOTE ABORT SWITCH with a smaller payload than any other on this surface — so the identical reasoning
 * that forbids a `DCHECK` on a malformed chunk forbids a `CHECK` on an allocation whose size those same bytes
 * chose. `INFLATE_REFUSED_NO_ROOM` is the declared absence for it, and a caller reads it exactly as it reads
 * a bad CRC: this datastream did not decode.
 * WHAT THIS DOES NOT DO IS CAP THE OUTPUT, and that is CLAUDE.md §NO BOUNDS rather than an omission. A limit
 * on how much a decode may produce is a decision that work will not happen, and it is not this component's to
 * take: the shape below makes STOPPING possible without one, because a caller that wants to stop simply does
 * not call the next step. Where the format's own OWNER states an expected length — PNG does, since IHDR fixes
 * a datastream's uncompressed size exactly — that is a SPEC check belonging to the component that reads IHDR,
 * and it is a refusal about the bytes rather than a budget.
 *
 * ── WHY A STEP MACHINE AND NOT A FUNCTION ────────────────────────────────────────────────────────────────
 * CLAUDE.md §scheduler: every code flow at any depth must suspend and resume, never drive to completion. A
 * decompressor called as one function is an unbounded stretch inside a single C activation — nothing can
 * raise the yield bit, nothing polls it, no sibling flow runs and no document interleaves for as long as it
 * takes. core/crypto/secure_hash.h is the settled precedent in this tree and states the same reason: it feeds
 * one message block per turn and yields between them, and its fixture takes every split of a vector because
 * a resume that changed the answer would make a digest depend on where the scheduler preempted.
 * SO THE STATE IS EXPLICIT AND THE DECODER IS RESUMABLE BY CONSTRUCTION. There is ONE implementation:
 * `inflate_step` advances an `Inflate` and says whether there is more. `inflate_run` is a loop over it and
 * adds no second decoder — a caller with nothing to interleave uses it, and a caller that must park between
 * turns drives `inflate_step` itself. A run-to-completion decoder landed first and converted later would be
 * the dual system §A-superseded-system-is-DELETED forbids, so it is not landed first.
 * THE GRANULARITY IS A POLICY INPUT AND NOT A CONSTANT THIS FILE HIDES, which is §NO BOUNDS' own distinction:
 * a bound decides work will not happen, a granularity decides only how often the work OFFERS to rest, and an
 * offer declined costs one predicate. `budget` is the number of OUTPUT BYTES a step may produce before it
 * returns `INFLATE_MORE`; the caller owns it, and zero means the default.
 *
 * NAMED RESIDUAL — THE INPUT IS COMPLETE AND A STEP NEVER WAITS FOR MORE OF IT. WHAT IS NOT COVERED: a
 * compressed stream still arriving, so that a step could exhaust the bytes in hand without the stream being
 * truncated. This decoder reads its whole input from `inflate_init` and answers `INFLATE_REFUSED_TRUNCATED`
 * for a stream that ends mid-symbol, which is correct for a complete body and wrong for a partial one. WHAT
 * THE NEXT DIFF BUILDS: a third `InflateStatus` arm meaning "the bytes in hand are spent and the stream has
 * not ended", plus an entry that hands a step more input, so a caller streaming a body can decode as it
 * arrives. HOW ITS ABSENCE WOULD SHOW: a caller that fed a prefix of a valid stream would read a REFUSAL
 * naming truncation, where the same bytes followed by their remainder decode; observable as a decode that
 * succeeds or fails depending on how a body was cut up before it reached this file.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_IMAGE_INFLATE_H
#define ENGINE_HOST_BROWSER_CORE_IMAGE_INFLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* WHAT A STEP ANSWERS. Two of these are states of a healthy decode and the rest are STATEMENTS ABOUT THE
   BYTES — a refusal is never a claim that this engine went wrong, which is why each one names what the stream
   did rather than what this file failed to do. They are separate arms rather than one `INFLATE_REFUSED`
   because a caller that reports why a resource was rejected can only report what it was told, and because a
   fixture asserting the RIGHT refusal is the only thing that distinguishes a decoder that refuses correctly
   from one that refuses everything. */
typedef enum {
    INFLATE_DONE = 0,                 /* RFC 1951 §3.2.3's last block completed, and the wrapper agreed */
    INFLATE_MORE,                     /* the step's budget was spent; the state is intact, call again */
    INFLATE_REFUSED_TRUNCATED,        /* the bytes ended inside something the format says continues */
    INFLATE_REFUSED_BLOCK_TYPE,       /* RFC 1951 §3.2.3's BTYPE 11, which that section names "reserved (error)" */
    INFLATE_REFUSED_STORED_LENGTH,    /* RFC 1951 §3.2.4: NLEN is not LEN's one's complement */
    INFLATE_REFUSED_CODE_LENGTHS,     /* RFC 1951 §3.2.7's code length sequence is over- or under-subscribed, or runs
                                         past the HLIT+HDIST+258 values that section says it describes */
    INFLATE_REFUSED_SYMBOL,           /* no code in the block's own alphabet denotes the bits that arrived */
    INFLATE_REFUSED_DISTANCE,         /* RFC 1951 §3.2.3: "a distance cannot refer past the beginning of the output
                                         stream" — or a length/distance code RFC 1951 §3.2.5's tables do not define */
    INFLATE_REFUSED_WRAPPER,          /* RFC 1950 §2.2's CMF/FLG: not deflate, too large a window, a preset
                                         dictionary, or a pair that is not a multiple of 31 */
    INFLATE_REFUSED_CHECKSUM,         /* RFC 1950 §2.2's ADLER32 disagrees with what the data decoded to */
    INFLATE_REFUSED_NO_ROOM           /* the output this stream asks for could not be allocated — see the
                                         header's own paragraph for why this is a refusal and not a CHECK */
} InflateStatus;

/* WHICH FRAMING THE BYTES CARRY. RFC 1951 is the compressed data alone; RFC 1950 wraps it in two header bytes
   and a trailing Adler-32, and PNG §10.1 "Compression method 0" is that wrapper — an IDAT datastream is a zlib
   stream, not a bare DEFLATE one. Both are named here rather than the wrapper being a separate component,
   because the wrapper's only possible consumer is the decompressor underneath it and a six-byte framing in a
   file of its own is a boundary with nothing on one side of it. */
typedef enum {
    INFLATE_RAW = 0,   /* RFC 1951 §3.2 "Compressed block format" and nothing around it */
    INFLATE_ZLIB       /* RFC 1950 §2.2 "Data format": CMF, FLG, the deflate data, ADLER32 */
} InflateWrapping;

/* HOW MANY OUTPUT BYTES A STEP PRODUCES BEFORE IT OFFERS TO REST, when a caller states no preference. It is a
   DEFAULT and not a limit: a step that reaches it returns `INFLATE_MORE` and the next step continues from the
   same bit. The number is the one RFC 1951 §3.2.5's length alphabet makes natural — 258 is the longest single
   match that section defines, so a budget of 64 of them is a rest offered at least every few matches and at
   most one match late, without a step ever being so short that the offer costs more than the work. */
#define INFLATE_DEFAULT_BUDGET 16512

/* THE DECODER'S WHOLE STATE. It is a plain struct with no pointer into anything but its own output and the
   caller's input, so a caller may park it, and every field is either a position in the input, a position in
   the output, or a table this file built from the block it is standing in.
   THE OUTPUT IS ALSO THE WINDOW, which is why there is no separate 32 KiB ring here. RFC 1951 §3.2.3 says a
   distance may cross block boundaries but "cannot refer past the beginning of the output stream", so with the
   whole output held contiguously the window IS the output and a copy is a read from it. A ring buffer is what
   a STREAMING-OUTPUT decoder needs, and this one does not stream its output. */
typedef struct {
    /* THE INPUT, BORROWED. The caller owns these bytes and must keep them alive for the decode's lifetime —
       asserted at every step, because a caller handing over a buffer it then frees is this codebase's own
       logic being wrong and is exactly what a DCHECK is for. */
    const uint8_t *in;
    size_t         in_len;
    size_t         in_pos;      /* the next byte the bit reader will pull */

    /* RFC 1951 §3.1.1 "Packing into bytes"' bit stream, least-significant bit of each byte first. */
    uint32_t       bitbuf;
    unsigned       bitcnt;

    /* THE OUTPUT, OWNED. `inflate_free` releases it, and `inflate_take` hands it to a caller instead. */
    uint8_t       *out;
    size_t         out_len;
    size_t         out_cap;

    /* HOW MANY OUTPUT BYTES THIS STEP MAY STILL PRODUCE — the policy input above, re-charged per step. */
    size_t         budget;

    InflateWrapping wrapping;

    /* WHERE THE DECODE IS STANDING. The enum is this file's own vocabulary rather than anything a standard
       names, so it lives in the .c: a caller has no business branching on it, and a second consumer of a
       resume point is a second thing free to disagree about what a state means. */
    int            phase;
    bool           final_block;     /* RFC 1951 §3.2.3's BFINAL of the block being decoded */

    /* RFC 1951 §3.2.4's non-compressed block, resumable mid-copy. */
    size_t         stored_left;

    /* AN INTERRUPTED RFC 1951 §3.2.5 MATCH. A single match is at most 258 bytes, so a step could always finish one —
       these exist so a step may rest INSIDE a long copy as well as between symbols, which is what keeps the
       offer rate independent of what the stream happens to contain. */
    size_t         copy_left;
    size_t         copy_dist;

    /* THE BLOCK'S TWO ALPHABETS, built per RFC 1951 §3.2.2's canonical rule. `count[l]` is how many codes have length
       l and `symbol[]` holds the symbols ordered by (length, symbol), which is the whole of what that
       section's construction needs and is what makes a decode a walk rather than a table lookup. */
    uint16_t       lit_count[16];
    uint16_t       lit_symbol[288];
    uint16_t       dist_count[16];
    uint16_t       dist_symbol[32];

    /* RFC 1950 §2.2's ADLER32, accumulated over the output as it is produced so the whole decode is never
       walked twice. Meaningless and unread under INFLATE_RAW. */
    uint32_t       adler_s1;
    uint32_t       adler_s2;
} Inflate;

/* BEGIN A DECODE over a complete compressed byte sequence. `bytes` is BORROWED and must outlive the decode;
   `n` may be zero, which is an ordinary truncated stream and not a hole. `budget` is the output-byte
   granularity a step rests at, and 0 means INFLATE_DEFAULT_BUDGET.
   IT ALLOCATES NOTHING, so it cannot fail and has no status to return: the output buffer is grown by the
   first step that produces a byte, which is also the first moment a refusal could be needed for it. */
void inflate_init(Inflate *z, const uint8_t *bytes, size_t n, InflateWrapping w, size_t budget);

/* ADVANCE THE DECODE. Returns INFLATE_MORE when the step's budget is spent and the state is intact, DONE when
   the stream has ended and any wrapper agreed, and a refusal otherwise. Once a refusal or DONE is returned
   the state is FINISHED: calling again is this codebase misusing its own component, and is DCHECKed. */
InflateStatus inflate_step(Inflate *z);

/* DRIVE IT TO AN ANSWER. Exactly `inflate_step` in a loop and no second decoder — for a caller with nothing
   to interleave, such as a fixture or a trusted-zone tool. A caller that must park between turns calls
   `inflate_step` and owns the loop itself; that is the whole difference and there is no other. */
InflateStatus inflate_run(Inflate *z);

/* THE DECODED BYTES, while the decoder still owns them. Valid after any step, including after INFLATE_MORE —
   a partial decode is a real prefix of the answer, which is what lets a caller observe progress. */
const uint8_t *inflate_bytes(const Inflate *z);
size_t         inflate_length(const Inflate *z);

/* TAKE THE OUTPUT. The caller becomes its owner and must `free` it; the decoder is left holding nothing, so
   `inflate_free` after a take is correct and releases nothing twice. Returns NULL only when nothing was ever
   produced, which a zero `*n` states positively rather than leaving to be inferred from the pointer. */
uint8_t *inflate_take(Inflate *z, size_t *n);

/* RELEASE whatever the decoder still owns. Idempotent, and safe on a decoder that refused. */
void inflate_free(Inflate *z);

/* RFC 1950 §2.2's ADLER32, exposed because the PNG datastream is not the only place this tree will need it
   and because a checksum with exactly one caller inside one file is a checksum nothing can test on published
   vectors. `s` starts at 1, which that section states ("s1 is initialized to 1, s2 to zero") and which is the
   one thing about Adler-32 that a reader is most likely to get wrong. */
uint32_t inflate_adler32(uint32_t s, const uint8_t *bytes, size_t n);

#endif
