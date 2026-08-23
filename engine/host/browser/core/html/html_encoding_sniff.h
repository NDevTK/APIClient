/* HTML §13.2.3.2 "Determining the character encoding" — THE ENCODING SNIFFING ALGORITHM. See the .c.
 *
 * WHAT IT IS FOR. DOM §4.5 Interface Document: "The characterSet, charset, and inputEncoding getter steps are
 * to return this's encoding's name." DOM gives every document a DEFAULT — "unless stated otherwise, a
 * document's encoding is the utf-8 encoding" — and THIS is the algorithm that states otherwise. Without it a
 * document served `windows-1252`, or carrying `<meta charset=shift_jis>`, answers UTF-8 for a member no
 * browser answers UTF-8 for, and — worse than the wrong string — is DECODED as UTF-8, so every byte above 0x7F
 * in it becomes U+FFFD before the tree builder sees it.
 *
 * IT IS OVER BYTES AND OUT-OF-BAND METADATA, WHICH IS WHY IT IS NOT THE PARSER'S. §13.2.3.2's own wording:
 * "this algorithm takes as input any out-of-band metadata available to the user agent (e.g. the Content-Type
 * metadata of the document) and all the bytes available so far". Both of those belong to the NAVIGATION, not to
 * the tree construction that follows — which is why this is a component the loader calls before it opens a
 * parse, and why every input arrives as a parameter rather than being read back off a half-built Document.
 *
 * THE CONFIDENCE IS NOT RETURNED, AND THAT IS A DESIGN STATEMENT RATHER THAN A GAP. §13.2.3.2 "returns a
 * character encoding and a confidence that is either tentative or certain", and the confidence has EXACTLY ONE
 * reader in the whole standard: §13.2.3.4 "Changing the encoding while parsing", whose trigger — a `<meta>`
 * declaration met by the tree builder while "the confidence is currently tentative" — this engine does not
 * have. A second half produced for nobody is the defect CLAUDE.md names as a field that is written and never
 * read: it looks like state, it is indistinguishable from a measurement, and the day someone reads it they will
 * read a value nothing ever checked. The diff that builds §13.2.3.4's reader is the diff that grows this
 * signature, and every `return` below already carries the confidence its step assigns in its own comment. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_ENCODING_SNIFF_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_ENCODING_SNIFF_H

#include <stddef.h>

/* §13.2.3.2's algorithm, in its own step order, over `n` bytes of the resource and the response's
   `Content-Type` header value (Fetch §2.2's "get" already joined the list's duplicates; NULL is a response
   that carried none). Answers an id in the Encoding registry (core/encoding/encoding.h) — never a failure,
   because the algorithm's last step is a default and therefore always answers.

   `parent_encoding` IS §13.2.3.2's CONTAINER-DOCUMENT STEP, SPLIT WHERE THE FACTS LIVE: "If parentDocument's
   origin is same origin with d's origin and parentDocument's character encoding is not UTF-16BE/LE, then
   return parentDocument's character encoding". The FIRST half — is there a container document, and are the two
   origins the same — is a fact about the NAVIGATION that the caller already holds, so the caller answers it by
   passing that document's encoding or -1. The SECOND half is a fact about the ENCODING and is applied inside,
   because a caller re-stating which encodings a document cannot be parsed in is a second place for that to be
   true. Reaching back into the navigable tree from here for either would be this component reading an input
   the operation already had, at a later time than the operation had it. */
int html_encoding_sniff(const char *bytes, size_t n, const char *content_type_value, int parent_encoding);

/* §13.2.3.2's "prescan a byte stream to determine its encoding", exported because it is a step of OTHER
   standards' algorithms too and not only of the one above: XHR §3.6.6 "set a document response" step 5 runs
   the final encoding, then this, then UTF-8 — a different order over a different metadata source, so a caller
   that could only reach the whole sniffing algorithm would have to re-state XHR's order inside it.
   Answers an encoding id, or -1 for the algorithm's "terminates unsuccessfully" — which, per §13.2.3.2, is
   already the result of "get an XML encoding" applied to the same bytes, since the prescan's own abort step
   says so. The caller passes the WINDOW it wants scanned (the standard encourages the first 1024 bytes and the
   authoring conformance requirements limit a declaration to them). */
int html_prescan_byte_stream(const char *bytes, size_t n);

/* HTML §2.5.3 "Extracting character encodings from meta elements" — "the algorithm for extracting a character
   encoding from a meta element, given a string s". It answers an encoding id or -1 for the standard's
   "nothing", and it takes BYTES because both of its callers hold bytes: the prescan above holds an attribute
   value it read out of a byte stream, and §13.2.6.4.4 'The "in head" insertion mode' holds a `content`
   attribute whose value the DOM stores as UTF-8. It is a DIFFERENT algorithm from `get an encoding` and the
   standard says why — "this algorithm is distinct from those in the HTTP specifications (for example, HTTP
   doesn't allow the use of single quotes …)" — so a caller must never reach for the other one. */
int html_extract_encoding_from_meta(const char *s, size_t n);

#endif
