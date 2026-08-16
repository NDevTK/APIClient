/* TextEncoder AND TextDecoder — the Encoding Standard §7. See encoding.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_ENCODING_ENCODING_H
#define ENGINE_HOST_BROWSER_CORE_ENCODING_ENCODING_H
#include "quickjs.h"

#include <stdbool.h>
#include <stddef.h>

void encoding_init(JSContext *ctx);
void encoding_install_protos(JSContext *ctx);   /* §7.1's and §7.2's prototypes, for ONE realm */
void encoding_install(JSContext *ctx, JSValueConst global);
void encoding_free(JSContext *ctx);

/* ---- WHAT §7.5's TextDecoderStream AND §7.6's TextEncoderStream REACH THIS COMPONENT THROUGH ---------------
 *
 * "Set stream's decoder to a new instance of encoding's decoder" is §7.5's own wording, and it is the SAME
 * decoder object §7.1 holds — one that keeps its half-read sequence between chunks. The streaming interfaces
 * are a TransformStream driving that object, so the codec is exported rather than written twice. */
typedef struct EncDecoder EncDecoder;

/* §4.2's "get an encoding": the encoding's id for `label`, or -1 for failure. The `replacement` encoding has
   an id like any other — REFUSING it is the caller's step, which §7.1's and §7.5's constructors both take and
   §4.2 itself does not. */
int  encoding_lookup(const char *label, size_t len);
bool encoding_is_replacement(int enc);
/* §4.1's name, already lowercased — what both `encoding` attributes answer with. */
const char *encoding_name_of(int enc);

EncDecoder *enc_decoder_new(int enc, bool fatal, bool ignore_bom);
void        enc_decoder_free(EncDecoder *d);
bool        enc_decoder_fatal(const EncDecoder *d);
bool        enc_decoder_ignore_bom(const EncDecoder *d);
int         enc_decoder_encoding(const EncDecoder *d);

/* Run `len` bytes through the decoder. `stream` false performs the END-OF-STREAM flush, which is what turns an
   incomplete held sequence into U+FFFD (or, in fatal mode, into the TypeError). Answers a string, or
   JS_EXCEPTION with that TypeError live. */
JSValue enc_decoder_decode(JSContext *ctx, EncDecoder *d, const uint8_t *p, size_t len, bool stream);

/* The bytes of a BufferSource whose union the DECLARATION has already brand-tested. `*pbuf` is a value the
   caller frees once it is done with the pointer. Returns 0, or -1 with an exception live. */
int encoding_buffer_source(JSContext *ctx, JSValueConst v, const uint8_t **pp, size_t *plen, JSValue *pbuf);

/* ---- §6's HOOKS FOR STANDARDS ------------------------------------------------------------------------------
 *
 * "For decoding, UTF-8 decode is to be used by new formats. For identifiers or byte sequences within a format
 * or protocol, use UTF-8 decode without BOM or UTF-8 decode without BOM or fail." Those callers are other
 * standards' algorithms, not the JS interfaces: they hold their strings as UTF-8 BYTES and run in C with no
 * realm to throw into, so the hook is over bytes and answers bytes. It is the SAME decoder §7.1 drives, because
 * a second one would drift from it at exactly the edges the standard is about (an overlong form, a surrogate
 * spelled in three bytes, a sequence the input ends in the middle of).
 *
 * WHICH ONE A CALLER RUNS IS THE CALLING STANDARD'S CHOICE, NOT A PREFERENCE, and the two differ by exactly the
 * three bytes of §6's step 2. A caller therefore quotes the algorithm its own step names — HTML §7.4.2.3.2's
 * javascript: URL links `#utf-8-decode`, URL §3.5's host parser and §5.1's urlencoded parser link
 * `#utf-8-decode-without-bom` — and neither hook is a stand-in for the other. */

/* "To UTF-8 decode an I/O queue of bytes ioQueue …: Let buffer be the result of peeking three bytes from
   ioQueue, converted to a byte sequence. If buffer is 0xEF 0xBB 0xBF, then read three bytes from ioQueue. (Do
   nothing with those bytes.) Process a queue with an instance of UTF-8's decoder, ioQueue, output, and
   "replacement". Return output." Steps 3-4 ARE the without-BOM hook below, so this is the peek, the discard,
   and then that hook — a second drive of the decoder is how the two would come to disagree. Same answer shape:
   malloc'd, NUL-terminated, WELL-FORMED UTF-8, with `*out_n` its length. */
char *encoding_utf8_decode(const char *p, size_t n, size_t *out_n);

/* "To UTF-8 decode without BOM an I/O queue of bytes ioQueue …: Process a queue with an instance of UTF-8's
   decoder, ioQueue, output, and "replacement"." A malformed sequence therefore becomes U+FFFD rather than
   surviving as the raw byte it was. The BOM is NOT removed — dropping it is §7.1's own step over the decoded
   output, and its absence here is what the hook's name says. Returns a malloc'd, NUL-terminated sequence of
   WELL-FORMED UTF-8; `*out_n` is its length, which is not strlen when a U+0000 decoded. */
char *encoding_utf8_decode_without_bom(const char *p, size_t n, size_t *out_n);

/* Infra's "scalar value string" — a string whose code points are all scalar values — over the UTF-8 BYTES a C
   component holds one as, which it is exactly when those bytes are well-formed UTF-8. This is what a standard's
   own `Assert: … is a scalar value string` is written as, so it is a DCHECK's condition: it allocates a scratch
   buffer and frees it, and touches nothing the program can observe. */
bool encoding_is_scalar_value_string(const char *p, size_t n);

#endif
