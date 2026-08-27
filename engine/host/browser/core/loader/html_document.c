/* HTML §7.5.2 "Loading HTML documents", and nothing else.
 *
 * §7.5.2's own steps are "Let document be the result of creating and initializing a Document object given
 * "html", type, and navigationParams", "If document's URL is about:srcdoc, then set document's mode to
 * no-quirks", "Create an HTML parser and associate it with the document", and then the fetch's tasks filling
 * the parser's input byte stream. The Document is created by the caller in this engine, and the parser and
 * the stream are core/html/html_parse.h's — so what this file adds is the refusal, and that is deliberate.
 *
 * A LOADER WHOSE BODY IS ONE CALL IS STILL WHERE THE ASSERT BELONGS. CLAUDE.md §Browser half: the parser must
 * be unreachable for a type it does not serve, ASSERTED WHERE THE PARSE IS, so that a route added later fires
 * instead of silently widening the old wrong answer. Asserting it at the dispatch instead is vacuous — the
 * dispatch just decided — and asserting it inside core/html/html_parse.h is wrong, because that entry serves
 * HTML §13.4 "Parsing HTML fragments", §8.5.1 `parseFromString`, §4.5.1 `createHTMLDocument` and XHR's
 * responseXML, none of which has a response or a computed type at all. The one place both facts are in hand is
 * a loader that takes the type and parses. That is this. */
#include "core/html/html_parse.h"
#include "core/loader/document_load_type.h"
#include "core/loader/html_document.h"
#include "core/mime/mime_type.h"
#include "check.h"

lxb_status_t html_document_load(lxb_html_document_t *document, DomParseRootKind root_kind,
                                const MimeType *type, const lxb_char_t *html, size_t size)
{
    DCHECK(document != NULL,
           "HTML §7.5.2 was reached with no Document — its step 1 creates one and hands it to the parser, so "
           "a loader with nothing to load into never ran that step");
    DCHECK(html != NULL,
           "HTML §7.5.2 was handed a NULL pointer for the response's characters — an empty HTML document is a "
           "zero SIZE over a real pointer, and §13.2.7 \"The end\" still builds html, head and body out of the "
           "EOF token for it");
    DCHECK(document_load_type_of(type) == DOC_LOAD_HTML,
           "HTML §13.2's parser was reached with a response HTML §7.4.5's load-a-document sends to some other "
           "§7.5 subsection — the arm is a fact about the COMPUTED TYPE and this loader re-asks it, so this is "
           "a route into the HTML parse that never asked what it fetched");
    return html_parse_document(document, root_kind, html, size);
}
