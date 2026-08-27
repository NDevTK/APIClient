/* WHICH DOCUMENT A RESPONSE LOADS AS — HTML §7.4.5 "Populating a session history entry"'s "load a document"
   type dispatch. See document_load_type.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_LOADER_DOCUMENT_LOAD_TYPE_H
#define ENGINE_HOST_BROWSER_CORE_LOADER_DOCUMENT_LOAD_TYPE_H

#include <stddef.h>

#include "core/fetch/headers.h"
#include "core/mime/mime_type.h"

/* §7.4.5's ARMS, one value per `<dt>` of its "if the type is one of the following types" list, in the order the
   list is written. THE ORDER IS THE SPEC AND NOT AN IMPLEMENTATION DETAIL: `image/svg+xml` is BOTH an XML MIME
   type and a supported image type, and it is the XML arm that claims it because XML is written first — an
   engine that tested the image group first would render SVG as a media document and never build its DOM.
   Each value names the §7.5 subsection that loads it, because that subsection is what a caller with no loader
   for this arm has to build, and a `@WHY` is read at the site. */
typedef enum {
    DOC_LOAD_HTML,       /* §7.5.2 Loading HTML documents */
    DOC_LOAD_XML,        /* §7.5.3 Loading XML documents */
    DOC_LOAD_TEXT,       /* §7.5.4 Loading text documents */
    DOC_LOAD_MULTIPART,  /* §7.5.5 Loading multipart/x-mixed-replace documents */
    DOC_LOAD_MEDIA,      /* §7.5.6 Loading media documents */
    DOC_LOAD_EXTERNAL    /* §7.4.5's final "Otherwise" — external software, or §7.5.7's DOM-less inline content */
} DocumentLoadType;

/* §7.4.5's "Let type be the computed type of navigationParams's response", classified into the arm that loads
   it. `computed` IS that computed type — mimesniff §7 "Determining the computed MIME type of a resource"'s
   answer, which §8.1 "Sniffing in a browsing context" makes the algorithm a navigation runs, produced by
   core/mime/mime_sniff.h.
   IT TAKES A RECORD AND NOT A HEADER VALUE, and the difference is the whole algorithm. It used to take the
   response's `Content-Type` string and run Fetch §2.2.2 "Headers"'s extraction on it, which is not what §7.4.5 says:
   "the computed type" is not the SUPPLIED type, and the gap between them is every response a server labelled
   nothing, `text/plain`, or `application/unknown` — for which the extraction answers failure or a generic and
   §7 answers what the BYTES are. A response with no `Content-Type` therefore reached the `Otherwise` arm and
   its crash, which is why a corpus handler that sets no header aborted the run.
   NEITHER HALF OF THAT IS SOFTENED BY THE CHANGE: the computed type is never undefined (mime_sniff.h states
   why), so "I could not tell" no longer exists as an answer, and nothing here defaults to HTML — a computed
   `application/octet-stream` is still DOC_LOAD_EXTERNAL and still crashes by name. */
DocumentLoadType document_load_type_of(const MimeType *computed);

/* §7.4.5's OWN FIRST STEP — "Let type be the computed type of navigationParams's response" — over the RESPONSE,
   which is the form every loader actually holds: a header list and the bytes. It runs MIME Sniffing §5.1
   "Interpreting the resource metadata" over the header list, Fetch §3.6 "`X-Content-Type-Options` header"'s
   determine-nosniff over the same list, and §7 "Determining the computed MIME type of a resource" over §5.2's
   resource header, and it answers the record `document_load_type_of` takes.
   IT IS HERE BECAUSE THE OTHER HALF OF THIS ALGORITHM IS, and a loader that has only the second half computes
   the first one itself. That is not a hypothetical: the type dispatch was reachable from ONE loader while a
   second one handed every response it fetched straight to the HTML parser, so an XML document reported its
   failure as a JavaScript COMPILE error — `<script><![CDATA[` is XML §2.7 "CDATA Sections" under an XML parser
   and, under HTML §13.2.6.4.4 The "in head" insertion mode, which switches the tokenizer to §13.2.5.4 Script
   data state, it is program text. One absent capability, two unrelated-looking names, and the misleading one
   pointed at the JavaScript compiler for a document that was never JavaScript.
   `out` is a record the CALLER FREES (mime_type_free), on every path, exactly like mime_sniff_computed's.
   `body` is the response's bytes and is never NULL: a caller with no response is not asking this question —
   HTML §7.4's initial about:blank has no response to compute a type from and is an HTML document by §7.4
   rather than by anything defaulting here. `body_len` is the WHOLE body; §5.2's 1445-byte resource header is
   taken from it here, so no caller has to know that number. */
void document_load_computed_type(MimeType *out, const HeaderList *response_headers,
                                 const void *body, size_t body_len);

/* The §7.5 subsection that loads this arm, number and title, for the crash at a caller that has no loader for
   it. Never NULL — every arm has one, and a caller formatting an arm this does not know has asked the wrong
   question. */
const char *document_load_type_section(DocumentLoadType t);

#endif
