/* WHICH DECODER A RESPONSE'S BYTES NEED — HTML §13.2.3.2 "Determining the character encoding", and the
   Encoding §6.1 "Legacy hooks for standards" decode that answer names. See document_load_decode.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_LOADER_DOCUMENT_LOAD_DECODE_H
#define ENGINE_HOST_BROWSER_CORE_LOADER_DOCUMENT_LOAD_DECODE_H

#include <stddef.h>

#include "core/fetch/headers.h"

/* §13.2.3.2 OVER A RESPONSE, FOLLOWED BY THE DECODE ITS ANSWER NAMES, as ONE operation — because they are one
 * question ("what do these bytes say") and because splitting them is how one of them comes to be asked without
 * the other. Answers the encoding id (core/encoding/encoding.h) and writes the decoded characters to `*out`.
 *
 * `*out` IS MALLOC'D, NUL-TERMINATED, WELL-FORMED UTF-8 AND THE CALLER FREES IT, which is `encoding_decode`'s
 * own answer shape carried out unchanged; `*out_len` is its length in bytes. The return is never negative:
 * §13.2.3.2's last step is a default, so the algorithm always answers.
 *
 * IT TAKES THE HEADER LIST AND NOT A `Content-Type` VALUE, for the reason
 * core/loader/document_load_type.h's `document_load_computed_type` takes one: an entry that can only reach the
 * second half of a classification computes the first half itself, and then one question has two answers. The
 * two components read this ONE header DIFFERENTLY and both are right, which is exactly why neither may read it
 * for the other — §13.2.3.2 runs on Fetch §2.2.2 "Headers"' `get`, which JOINS the list's values with ", ",
 * while MIME Sniffing §5.1 "Interpreting the resource metadata" takes the LAST value UNJOINED because §5's
 * check-for-apache-bug flag is a byte-exact comparison a joined list can never satisfy. Two reads of one
 * header, each inside the component whose algorithm defines it.
 *
 * `body` IS THE RESPONSE'S BYTES AND IS NEVER NULL. A caller with no response is not asking this question:
 * HTML §7.4 "Navigation and session history"'s initial `about:blank` has no bytes to sniff and no
 * `Content-Type` to be overruled by, and its
 * Document keeps DOM §4.5 "Interface Document"'s default. `body_len` may be zero — an empty response is still
 * a response, and §13.2.3.2 still answers for it.
 *
 * `parent_encoding` IS §13.2.3.2's CONTAINER-DOCUMENT STEP, passed straight through to
 * core/html/html_encoding_sniff.h, which states in full why the step is split at this boundary: whether there
 * IS a container document and whether the two origins are same origin are facts about the NAVIGATION that only
 * the caller holds, so the caller answers them by passing that document's encoding or -1.
 *
 * WHAT THIS IS FOR, AND IT IS NOT A TIDY-UP OF A WORKING PATH. The sniff-then-decode pair was open-coded at
 * ONE of the entries that builds a Document out of a response, and the other two handed the response's own
 * bytes to a tokenizer that reads UTF-8 — which is HTML §13.2.3.1 "Parsing with a known character encoding"
 * ("When the HTML parser is to operate on an input byte stream that has a known definite encoding, then the
 * character encoding is that encoding and the confidence is certain") applied with an encoding nobody
 * determined. That is not an absent capability and it does not crash: a `windows-1252` document reaches the
 * tree builder with every byte above 0x7F already turned into U+FFFD, and `document.characterSet` answers
 * UTF-8 for a document no browser answers UTF-8 for. CLAUDE.md §Browser half: a question some entries ask and
 * others do not is one missing capability wearing two names, and the name you meet first is the wrong one. */
int document_load_decode(char **out, size_t *out_len,
                         const HeaderList *response_headers,
                         const void *body, size_t body_len,
                         int parent_encoding);

#endif
