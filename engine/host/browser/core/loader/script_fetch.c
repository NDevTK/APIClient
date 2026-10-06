/* HTML §8.1.4.2 "Fetching scripts" — THE DECODE, which is the step at which a fetched byte sequence becomes a
 * script's SOURCE TEXT.
 *
 * WHY IT IS A COMPONENT AND NOT A LINE AT THE COMPILER. §8.1.4.2 has two entries that both end in a fetched
 * body, and they run DIFFERENT algorithms on it:
 *
 *   fetch a classic script       "Let sourceText be the result of DECODING bodyBytes to Unicode, using
 *                                 encoding as the fallback encoding."   [Encoding §6.1 #decode]
 *   fetch a single module script "Let sourceText be the result of UTF-8 DECODING bodyBytes."
 *                                                                       [Encoding §6  #utf-8-decode]
 *
 * They differ in two observable ways and neither is a detail. The classic entry honours a LABEL — the response's
 * `Content-Type` charset, run through Fetch §3.5's legacy extract an encoding — so `charset=windows-1252` makes
 * byte 0x92 a U+2019 where the module entry makes it a U+FFFD. And both entries drop a leading UTF-8 BOM, but
 * the classic one drops a UTF-16 BOM as well and switches decoder on it, because #decode's step 1 is a BOM
 * SNIFF over three rows where #utf-8-decode's is a three-byte comparison against one.
 *
 * WHAT RAN BEFORE THIS FILE EXISTED: neither algorithm, for any script this engine has ever fetched. The reply's
 * bytes went from the host's reply record to JS_FlowNew / JS_FlowEvalModule unchanged, which is wrong in both
 * directions at once. The two runs below are the SUBMODULE'S OWN WORDS and so go in BACKTICKS, the way
 * core/byte_reader.c and core/fetch/fetch.h write this same decoder: a quotation mark claims a STANDARD's
 * sentence, and the anchor nearest them is Fetch §3.5, so quickjs in quotation marks is judged against a
 * document it never came from — which is what the second of them used to be. Forward: quickjs's `next_token`
 * REFUSES a byte no UTF-8 sequence contains (`invalid UTF-8 sequence`), so one stray 0x81 anywhere in a
 * minified bundle answered a SyntaxError no browser produces and every endpoint in that chunk was lost
 * silently — a page reported with less API surface than it has. Backward: cutils.h says its decoder
 * implements Encoding's algorithm `except it accepts UTF-8 encoded surrogates as JavaScript allows them in
 * strings`, so `ED A0 80` compiled a LONE SURROGATE into a string where Encoding's decoder answers U+FFFD. A
 * fetched script is also an @S sink's input, and a breakout is constructed from the bytes that survive to the
 * sink: a decode that disagrees with the browser's mis-places every one of them.
 *
 * THE ERROR MODE IS "replacement" IN BOTH, which is what makes a malformed sequence a U+FFFD instead of an
 * abort, and it is why the postcondition below can be an assertion rather than a hope. */
#include "check.h"
#include "core/encoding/encoding.h"   /* §6's UTF-8 decode and §6.1's decode — the two algorithms, by name */
#include "core/mime/mime_type.h"      /* Fetch §3.5's extract a MIME type, and its legacy extract an encoding */
#include "core/loader/script_fetch.h"

/* ONE `reason` PER ENTRY, so the sentence a reader needs is which of §8.1.4.2's two algorithms was running.
   THE SCALAR-VALUE HALF OF THIS REASON IS RETIRED, AND THE RETIRED WORDING IS KEPT BECAUSE A READER WHO
   RE-DERIVES IT WILL RE-ADD THE SECOND DECODE. It read: "decoded a fetched body to something that is not a
   scalar value string — the step runs Encoding's <hook>, whose error mode is \"replacement\" and therefore
   answers U+FFFD for every malformed sequence, so this is a decoder contradicting its own error mode. What
   stands downstream is a compiler that refuses an ill-formed byte outright, so the page would lose its whole
   program to a SyntaxError no browser produces." Every clause of that is TRUE and the place to assert it was
   never here: it was a claim about the DECODER, asserted at one of its five consumers by RE-RUNNING the whole
   decoder over the whole output in fatal mode — a second complete pass, with its own allocation, over every
   byte of every script this engine fetches, in the DEV build that every measurement in this project is taken
   in. Encoding §4.1's process an item states the assertion at the step that PUSHES ("Assert: encoderDecoder
   is not a decoder instance or result does not contain any surrogates"), and core/encoding/encoding.c's
   enc_put is that push, so the invariant is now true by construction at the point of production.
   WHAT THAT TRADES IS THE ADDRESS, IN THE DIRECTION THAT HELPS: this site named which of §8.1.4.2's two
   entries was running and could not name the defect, because the defect is a decoder handler computing a
   value outside the scalar values; the macro at enc_put is expanded at each handler, so the abort now names
   the §8/§10/§11/§12/§13/§14 step that computed it. */
#define SCRIPT_FETCH_WHY(entry, hook) \
    "HTML §8.1.4.2's \"" entry "\" decoded a fetched body to NOTHING — the step runs Encoding's " hook ", " \
    "whose own contract is a malloc'd, NUL-terminated sequence and which CHECKs its allocation rather than " \
    "answering a null pointer, so this is that contract having changed under this file. What stands " \
    "downstream is a compiler handed a null source"

/* §8.1.4.2's RESPONSE-SIDE FAILURE TEST — see script_fetch.h for which three algorithms state it and why the
   predicate is over the STATUS alone. Fetch §2.2.3 "Statuses": "An ok status is a status in the range 200 to
   299, inclusive."
   NO ASSERT STANDS OVER `status`, AND THAT IS THE RULE RATHER THAN AN OMISSION. The number is one a stranger's
   server chose, so it is INPUT: §2.2.3 admits "an integer in the range 0 to 999, inclusive" and this engine's
   own network error is a status of 0 (Fetch §2.2.6 "Responses"), so there is no value here this codebase
   computed and therefore nothing an assertion could be about except a remote party's choice. The answer to a
   status this algorithm refuses is the algorithm's own failure — `onComplete` given null — and never a crash. */
bool script_fetch_status_ok(int status)
{
    return status >= 200 && status <= 299;
}

/* WHAT EVERY SOURCE TEXT THIS FILE PRODUCES IS, asserted once for both entries.
   THE WELL-FORMEDNESS ASSERT THAT STOOD HERE IS GONE AND IS NOT WEAKENED — it moved to the step that produces
   the bytes. It read `DCHECK(encoding_is_scalar_value_string(source, n), why)`, which runs Encoding's UTF-8
   decoder a SECOND time, in fatal mode, over the whole output of the first one. The verdicts are the same
   verdict and the equivalence is exact in both directions: core/encoding/encoding.c's enc_put emits the
   minimal UTF-8 form of whatever code point it is handed, so the output is ill-formed exactly when some code
   point reaching it was not a scalar value, and §8.1.1's machine refuses each of the three ways that can
   happen (a surrogate's `ED` lead bounded at 0x9F, a value past U+10FFFF against `F4`'s bound of 0x8F, and a
   lead of `F5` or more that no arm admits). So the assertion is now made ONCE, at Encoding §4.1's own step for
   it, for every decode in the engine rather than for the two entries below — and the abort it produces names
   the decoder handler that computed the value instead of naming which script was being fetched when a
   different component misbehaved.
   WHAT THIS DOES NOT TOUCH is the same construct at core/url/url.c, whose operands are the urlencoded parser's
   stored bytes rather than this decoder's output — that site's own comment says it is the serializer's assert
   about its PRODUCERS, one of which takes a USVString and never reaches enc_put at all. Same spelling, a
   different population, and the per-site read is what separates them. */
static char *script_fetch_source_text(char *source, size_t n, size_t *out_n, const char *why)
{
    DCHECK(source != NULL, why);
    if (out_n) *out_n = n;
    return source;
}

char *script_fetch_classic_source_text(const char *body_bytes, size_t n, const char *content_type,
                                       int fallback_encoding, size_t *out_n)
{
    MimeType mt;
    int encoding;
    size_t src_n = 0;
    char *source;

    DCHECK(body_bytes != NULL || n == 0,
           "a classic script's body reached the decode as a null pointer with a length — the two describe one "
           "byte sequence, and a caller with no body at all passes an empty one");
    /* Step 5.3: "Let potentialMIMETypeForEncoding be the result of extracting a MIME type given response's
       header list." A response with no `Content-Type` at all is Fetch's "values is null", which is FAILURE —
       and failure is a value step 5.4 takes, so it is passed on as one rather than short-circuited here.
       `mime_type_extract` initialises the record whichever way it answers, which is why one call covers both
       the extraction and this record's whole lifetime. */
    /* Step 5.4: "Set encoding to the result of legacy extracting an encoding given potentialMIMETypeForEncoding
       and encoding." The standard's note is what makes this correct for a bundle served under any of the dozen
       JavaScript MIME types, and under the wrong one: "this intentionally ignores the MIME type ESSENCE" — only
       the `charset` parameter is read, so `Content-Type: text/plain;charset=windows-1252` still decides the
       encoding of a script this algorithm does not MIME-check at all ("for historical reasons, this algorithm
       does not include MIME type checking, unlike the other script-fetching algorithms in this section"). */
    encoding = mime_type_legacy_extract_encoding(mime_type_extract(&mt, content_type) ? &mt : NULL,
                                                 fallback_encoding);
    mime_type_free(&mt);
    /* Step 5.5: "Let sourceText be the result of decoding bodyBytes to Unicode, using encoding as the fallback
       encoding", with the note "the decode algorithm overrides encoding if the file contains a BOM" — which is
       Encoding §6.1's own step 1-2 and the reason the whole byte sequence goes in rather than a BOM-trimmed
       one. A `<script src>` served as UTF-16 with its BOM is decoded as UTF-16 however the header labelled it. */
    source = encoding_decode(body_bytes ? body_bytes : "", n, encoding, &src_n);
    return script_fetch_source_text(source, src_n, out_n,
                                    SCRIPT_FETCH_WHY("fetch a classic script", "§6.1 decode"));
}

char *script_fetch_module_source_text(const char *body_bytes, size_t n, size_t *out_n)
{
    size_t src_n = 0;
    char *source;

    DCHECK(body_bytes != NULL || n == 0,
           "a module script's body reached the decode as a null pointer with a length — the two describe one "
           "byte sequence, and a caller with no body at all passes an empty one");
    /* Step 13.7.1: "Let sourceText be the result of UTF-8 decoding bodyBytes." §6's UTF-8 DECODE, whose steps
       1-2 peek three bytes and discard a UTF-8 BOM — not the without-BOM hook, which would leave the U+FEFF in
       the source, and not §6.1's decode, which has no label to be given. §6 states which caller gets which:
       "for decoding, UTF-8 decode is to be used by new formats. For identifiers or byte sequences within a
       format or protocol, use UTF-8 decode without BOM or UTF-8 decode without BOM or fail." A module's source
       is a format's text. */
    source = encoding_utf8_decode(body_bytes ? body_bytes : "", n, &src_n);
    return script_fetch_source_text(source, src_n, out_n,
                                    SCRIPT_FETCH_WHY("fetch a single module script", "§6 UTF-8 decode"));
}
