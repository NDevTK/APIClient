/* TextEncoder AND TextDecoder — the Encoding Standard §7. See encoding.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_ENCODING_ENCODING_H
#define ENGINE_HOST_BROWSER_CORE_ENCODING_ENCODING_H
#include "quickjs.h"

#include <stdbool.h>
#include <stddef.h>

void encoding_init(JSContext *ctx);
/* Encoding §7.2 Interface TextDecoder's and §7.4 Interface TextEncoder's prototypes, their Web IDL §3.7.1
   Interface object's interface objects, and the Web IDL §3.8 property references for them — for ONE realm,
   declared into core/realm.h's list. ONE entry because Web IDL §3.8 Platform objects implementing interfaces'
   `define the global property references` is given "target" and "a realm realm" and its step 1 population is
   "every interface that is exposed in realm": no Document appears in it. Both interfaces declare
   `[Exposed=*]`, so EVERY realm owes both names — and while the interface objects were installed from
   core/platform.c's per-document column, a worker realm, which reaches no platform_document_install, received
   neither.
   THE PAIR IS Encoding §7.2 AND §7.4, NEVER §7.2 AND §7.1: Encoding §7.1 Interface mixin TextDecoderCommon
   and §7.3 Interface mixin TextEncoderCommon are MIXINS, and Web IDL §3.7.1 Interface object is written of an
   interface — so a mixin has no interface object and Web IDL §3.8 defines no property reference for one. */
void encoding_install_realm(JSContext *ctx);
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
/* §4.2 Names and labels' NAME, in the standard's own case — "UTF-8", "Shift_JIS", "ISO-8859-8-I". The table
   there has TWO columns and this engine used to keep one, which is a defect that reads as a spelling choice
   and is not: DOM §4.5 Interface Document's characterSet/charset/inputEncoding return "this's encoding's
   name", so a Document decoded as Shift_JIS whose registry knows only `shift_jis` answers a string no browser
   produces, and HTML §4.10.22.4's `_charset_` entry carries the same name into a form submission. */
const char *encoding_name(int enc);
/* Encoding §7.1 Interface mixin TextDecoderCommon's `encoding` getter: "this's encoding's name, ASCII
   lowercased" — the OTHER of the two, and the reason it is a separate entry rather than a caller's tolower
   loop is that a caller choosing between them is a caller stating which spec line it is answering.
   §4.2 Names and labels also states "For each encoding, ASCII-lowercasing its name yields one of its labels",
   so this answers a LABEL out of the same table rather than a second column of strings — see encoding_table.h,
   whose generator FAILS if the standard's sentence ever stops holding. */
const char *encoding_name_ascii_lowercased(int enc);
/* §4.3 Output encodings' "get an output encoding": "If encoding is replacement or UTF-16BE/LE, then return
   UTF-8. Return encoding." It is here rather than at its callers because the standard names exactly who asks —
   "useful for URL parsing and HTML form submission, which both need exactly this" — and a caller that folded
   the three ids itself would be a second statement of which encodings cannot be OUTPUT. */
int encoding_output_encoding(int enc);

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
 * javascript: URL and HTML §8.1.4.2's "fetch a single module script" link `#utf-8-decode`, URL §3.5's host
 * parser and §5.1's urlencoded parser link `#utf-8-decode-without-bom` — and neither hook is a stand-in for the
 * other. §6.1's `decode` below is a THIRD algorithm and not a variant of either: HTML §8.1.4.2's "fetch a
 * classic script" is the caller that has a LABEL to honour, so its step links `#decode`.
 *
 * The rows, so a reader adding one can see which callers already exist:
 *   #decode                     fallback-encoding + BOM-overrides-label   HTML §8.1.4.2 fetch a classic script
 *   #utf-8-decode               BOM discarded, UTF-8 only                 HTML §7.4.2.3.2 javascript: URL,
 *                                                                        HTML §8.1.4.2 fetch a single module
 *                                                                          script
 *   #utf-8-decode-without-bom   BOM kept, UTF-8 only                      URL §3.5 host parser,
 *                                                                        URL §5.1 urlencoded parser */

/* §4.2's UTF-8, by id. A component that names an encoding LITERALLY asks the registry for it rather than
   writing down a table index, so there is exactly one authority on which encodings exist and what they are
   numbered. */
int encoding_utf8(void);

/* §6.1's BOM SNIFF: "Let BOM be the result of peeking 3 bytes from ioQueue, converted to a byte sequence. For
   each of the rows in the table below, starting with the first one and going down, if BOM starts with the bytes
   given in the first column, then return the encoding given in the cell in the second column of that row.
   Otherwise, return null." The three rows are 0xEF 0xBB 0xBF -> UTF-8, 0xFE 0xFF -> UTF-16BE, 0xFF 0xFE ->
   UTF-16LE. -1 is the standard's null. It is EXPORTED because the standard's own note says it is: "this hook is
   a workaround for the fact that decode has no way to communicate back to the caller that it has found a byte
   order mark … the hook is to be invoked before decode" — a caller that must know WHICH encoding was used runs
   it itself, and gets the same answer `decode` acts on because it is the same function. */
int encoding_bom_sniff(const char *p, size_t n);

/* §6.1's DECODE, the LEGACY hook: "To decode an I/O queue of bytes ioQueue given a fallback encoding encoding
   …: Let BOMEncoding be the result of BOM sniffing ioQueue. If BOMEncoding is non-null: set encoding to
   BOMEncoding; read three bytes from ioQueue, if BOMEncoding is UTF-8, otherwise read two bytes. (Do nothing
   with those bytes.) Process a queue with an instance of encoding's decoder, ioQueue, output, and
   "replacement". Return output."
   THE BOM IS MORE AUTHORITATIVE THAN THE LABEL, which the standard states as a deliberate violation — "for
   compatibility with deployed content, the byte order mark is more authoritative than anything else. In a
   context where HTTP is used this is in violation of the semantics of the `Content-Type` header" — so a caller
   that computed `fallback_encoding` from a charset parameter must still hand the WHOLE byte sequence over and
   let this overrule it. `fallback_encoding` is an id from this registry; the standard's callers get theirs from
   §4.2's get an encoding, which is what makes `replacement` a possible value here and a decodable one.
   Same answer shape as the two hooks above: malloc'd, NUL-terminated, WELL-FORMED UTF-8, `*out_n` its length. */
char *encoding_decode(const char *p, size_t n, int fallback_encoding, size_t *out_n);

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
