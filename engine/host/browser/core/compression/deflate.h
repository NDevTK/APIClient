/* DEFLATE compression: RFC 1951 "DEFLATE Compressed Data Format Specification version 1.3", framed as RFC 1951
 * alone, as RFC 1950 "ZLIB Compressed Data Format Specification version 3.3" or as RFC 1952 "GZIP file format
 * specification version 4.3" — the three formats the Compression Standard §3 "Supported formats" names
 * "deflate-raw", "deflate" and "gzip". It turns bytes into bytes and knows nothing about streams or realms.
 *
 * The encoder writes RFC 1951 §3.2.4 non-compressed blocks, which §1.4 Compliance admits from a compliant
 * compressor, and the framing's fields at RFC 1950 §2.3 / RFC 1952 §2.3.1.2 compliance defaults.
 * Named residual. Not covered: LZ77 matching and Huffman coding (RFC 1951 §3.2.5-§3.2.7), so the output is
 * longer than Chromium's zlib output for the same input, though every decoder reads it as the same bytes.
 * Next diff builds: zlib's level-6 match search and block choice, so a compressed body is the bytes Chromium
 * sends. Absence shows as: a compressed request body whose length differs from Chromium's capture of the same
 * upload. The state is plain data, so a caller's record holding it is captured per flow as bytes. */
#ifndef ENGINE_HOST_BROWSER_CORE_COMPRESSION_DEFLATE_H
#define ENGINE_HOST_BROWSER_CORE_COMPRESSION_DEFLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DEFLATE_RAW = 0,   /* RFC 1951 §3.2 "Compressed block format" and nothing around it */
    DEFLATE_ZLIB,      /* RFC 1950 §2.2 "Data format": CMF, FLG, the deflate data, ADLER32 */
    DEFLATE_GZIP       /* RFC 1952 §2.3 "Member format": one member, its header, the deflate data, CRC32, ISIZE */
} DeflateWrapping;

/* A growable output buffer the encoder appends to. The caller owns `b` and frees it with free(). */
typedef struct {
    uint8_t *b;
    size_t   n, cap;
} DeflateOut;

typedef struct {
    DeflateWrapping wrapping;
    bool     header_written;   /* the framing's header is in some earlier output */
    bool     finished;         /* deflate_finish ran; nothing more may be written */
    uint32_t check;            /* RFC 1950 §2.2's ADLER32 or RFC 1952 §2.3.1's CRC32 over the input so far */
    uint32_t isize;            /* RFC 1952 §2.3.1's ISIZE: the input's length modulo 2^32 */
} Deflate;

void deflate_init(Deflate *d, DeflateWrapping w);
/* Compress `n` bytes, appending to `out` the framing's header when none was written yet and non-final RFC 1951
   §3.2.4 blocks holding the bytes. Writes nothing for `n` 0. Precondition: not finished. */
void deflate_write(Deflate *d, const uint8_t *in, size_t n, DeflateOut *out);
/* End the stream: the header if none was written, a final empty block, and the framing's trailer. */
void deflate_finish(Deflate *d, DeflateOut *out);

#endif
