/* HTML §8.1.4.2 "Fetching scripts" — THE STEP AT WHICH A FETCHED BODY BECOMES A SCRIPT'S SOURCE TEXT.
 * See script_fetch.c.
 */
#ifndef ENGINE_HOST_BROWSER_LOADER_SCRIPT_FETCH_H
#define ENGINE_HOST_BROWSER_LOADER_SCRIPT_FETCH_H
#include <stddef.h>

/* "Fetch a classic script" step 5, the processResponseConsumeBody steps, given `bodyBytes`:
 *   5.4  "Let potentialMIMETypeForEncoding be the result of extracting a MIME type given response's header
 *         list."
 *   5.5  "Set encoding to the result of legacy extracting an encoding given potentialMIMETypeForEncoding and
 *         encoding." (with the standard's note: "this intentionally ignores the MIME type essence")
 *   5.6  "Let sourceText be the result of decoding bodyBytes to Unicode, using encoding as the fallback
 *         encoding." (with the standard's note: "the decode algorithm overrides encoding if the file contains
 *         a BOM")
 * `content_type` is the response header list's joined `Content-Type` value, or NULL when the response carried
 * none — which is the "values is null" that makes extract a MIME type answer failure, and step 5.5 then keep
 * `fallback_encoding`. `fallback_encoding` is the algorithm's `encoding` ARGUMENT, which HTML §4.12.1 computes
 * from the script element's `charset` attribute or, failing that, its node document's encoding.
 * Answers malloc'd, NUL-terminated, WELL-FORMED UTF-8; `*out_n` is its length, which is not strlen when the
 * source decoded a U+0000. */
char *script_fetch_classic_source_text(const char *body_bytes, size_t n, const char *content_type,
                                       int fallback_encoding, size_t *out_n);

/* "Fetch a single module script" step 12, the processResponseConsumeBody steps, given `bodyBytes`:
 *   12.7.1  "Let sourceText be the result of UTF-8 decoding bodyBytes."
 * A DIFFERENT ALGORITHM, not this file's other entry with an argument left out: a module script's source is
 * UTF-8 whatever the response says, so there is no MIME type to extract for an encoding and no label to
 * honour. §4.12.1 says so about the element too — "if el's type is `module`, this encoding will be ignored" —
 * and this is the one place in this engine where that sentence is what the code does rather than a note.
 * Same answer shape as the classic entry. */
char *script_fetch_module_source_text(const char *body_bytes, size_t n, size_t *out_n);

#endif
