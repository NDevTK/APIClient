/* HTML §8.1.4.2 "Fetching scripts" — THE TWO STEPS EVERY FETCHED SCRIPT GOES THROUGH: the one that REFUSES a
 * response, and the one at which a fetched body becomes a script's SOURCE TEXT.
 * See script_fetch.c.
 */
#ifndef ENGINE_HOST_BROWSER_LOADER_SCRIPT_FETCH_H
#define ENGINE_HOST_BROWSER_LOADER_SCRIPT_FETCH_H
#include <stdbool.h>
#include <stddef.h>

/* §8.1.4.2's RESPONSE-SIDE FAILURE TEST, WHICH ALL THREE OF ITS FETCH ALGORITHMS STATE IDENTICALLY and none of
 * them states differently. "Fetch a classic script" step 5.2, "fetch a classic worker script"'s
 * processResponseConsumeBody step 2 and "fetch a single module script" step 13.1 each open "If any of the
 * following are true:" over a two-item list whose members are `bodyBytes` "is null or failure" and
 * "response's status is not an ok status" — and each then answers null: the two classic entries with "then
 * run onComplete given null, and abort these steps", the module entry by removing the module map record and
 * "For each callback of callbacks: run callback given null".
 *
 * WHY THE PREDICATE IS OVER THE STATUS AND NOT OVER THE WHOLE DISJUNCT. The other member is about the BODY,
 * and a caller holding a reply record knows it in its own vocabulary — this engine's network error is
 * `JS_NULL` (core/fetch/fetch.h) and its callers already test for that. Answering both here would take a
 * reply record, which would make a §8.1.4.2 component depend on the shape of a host's reply; the caller
 * conjoins the two facts it holds. So a caller that tests this and forgets the body half has a hole, and the
 * one that tests the body half and forgets this is what this function exists because of.
 *
 * Fetch §2.2.3 "Statuses" is the definition and the only one: "An ok status is a status in the range 200 to
 * 299, inclusive."
 *
 * NAMED RESIDUAL — THIS IS FETCH'S PRIMITIVE UNDER HTML'S ROOF. WHAT IS NOT COVERED: `ok status` belongs to
 * Fetch §2.2.3 and is spelled a third time in core/fetch/response.c's `ok` getter, over a Response's own
 * `status` field rather than over a reply record, so this file holds a §8.1.4.2-scoped copy of a Fetch
 * concept and there are two right answers to one question in the tree. WHAT THE NEXT DIFF BUILDS: the range
 * test declared in core/fetch/fetch.h beside the other reply-record readers, with this function and
 * `js_response_get`'s magic-0 arm both routed to it and this declaration deleted. HOW ITS ABSENCE SHOWS: the
 * day Fetch's own definition moves (its §2.2.3 already carries "various edge cases in mapping HTTP/1's status
 * code to this concept are worked on in issue 1156"), the two spellings drift and `new Response(b,
 * {status:299}).ok` and a 299 `<script src>` disagree about the same sentence. */
bool script_fetch_status_ok(int status);

/* "Fetch a classic script" step 5, the processResponseConsumeBody steps, given `bodyBytes`:
 *   5.3  "Let potentialMIMETypeForEncoding be the result of extracting a MIME type given response's header
 *         list."
 *   5.4  "Set encoding to the result of legacy extracting an encoding given potentialMIMETypeForEncoding and
 *         encoding." (with the standard's note: "this intentionally ignores the MIME type essence")
 *   5.5  "Let sourceText be the result of decoding bodyBytes to Unicode, using encoding as the fallback
 *         encoding." (with the standard's note: "the decode algorithm overrides encoding if the file contains
 *         a BOM")
 * `content_type` is the response header list's joined `Content-Type` value, or NULL when the response carried
 * none — which is the "values is null" that makes extract a MIME type answer failure, and step 5.4 then keep
 * `fallback_encoding`. `fallback_encoding` is the algorithm's `encoding` ARGUMENT, which HTML §4.12.1 computes
 * from the script element's `charset` attribute or, failing that, its node document's encoding.
 * Answers malloc'd, NUL-terminated, WELL-FORMED UTF-8; `*out_n` is its length, which is not strlen when the
 * source decoded a U+0000.
 * THE SUB-NUMBERS WERE 5.4/5.5/5.6 AND WERE OFF BY ONE, which is §8.1.4.2's own nested list doing exactly what
 * a flat `<li>` count does: step 5.2's two disjuncts (the failure test `script_fetch_status_ok` is half of) are
 * items of a `<ul>` INSIDE one step, and counting them as peers promoted every step after them. The drift
 * therefore begins at 5.3 and 5.1/5.2 were right all along, which is why sampling the head of this list
 * confirmed it. citegen's step channel cannot see it either — that channel asks whether a step number CAN
 * exist in the section it names, and 5.4, 5.5 and 5.6 all can (step 5 holds eight). */
char *script_fetch_classic_source_text(const char *body_bytes, size_t n, const char *content_type,
                                       int fallback_encoding, size_t *out_n);

/* "Fetch a single module script" step 13, the processResponseConsumeBody steps, given `bodyBytes`:
 *   13.7.1  "Let sourceText be the result of UTF-8 decoding bodyBytes."
 * (12/12.7.1 stood here: step 12 is "set up the module script request" and step 13 is the fetch.)
 * A DIFFERENT ALGORITHM, not this file's other entry with an argument left out: a module script's source is
 * UTF-8 whatever the response says, so there is no MIME type to extract for an encoding and no label to
 * honour. §4.12.1 says so about the element too — "if el's type is `module`, this encoding will be ignored" —
 * and this is the one place in this engine where that sentence is what the code does rather than a note.
 * Same answer shape as the classic entry. */
char *script_fetch_module_source_text(const char *body_bytes, size_t n, size_t *out_n);

#endif
