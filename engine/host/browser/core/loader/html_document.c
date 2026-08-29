/* HTML §7.5.2 "Loading HTML documents", and nothing else.
 *
 * §7.5.2's own steps are "Let document be the result of creating and initializing a Document object given
 * "html", type, and navigationParams", "If document's URL is about:srcdoc, then set document's mode to
 * no-quirks", "Create an HTML parser and associate it with the document", and then the fetch's tasks filling
 * the parser's input byte stream. The Document is created by the caller in this engine, and the parser and
 * the stream are core/html/html_parse.h's — so what this file adds is the refusal and the SHAPE OF THE FILL,
 * and both are deliberate.
 *
 * THE FILL IS THE HALF THAT WAS MISSING. This file used to be one call to `html_parse_document`, which opens
 * the stream, hands lexbor the whole response and emits the EOF — one un-interruptible span the length of the
 * document, inside a flow's slice, for a quantity CLAUDE.md §scheduler names as the thing that must suspend at
 * any depth. §7.5.2 does not describe a single call and never did: it describes a task per arrival of bytes
 * and a task for the implied EOF, which is a pull. The header states the citation; what this file holds is the
 * position in the response, which is the only state a fill has between two of those tasks.
 *
 * A LOADER WHOSE BODY IS ONE CALL IS STILL WHERE THE ASSERT BELONGS. CLAUDE.md §Browser half: the parser must
 * be unreachable for a type it does not serve, ASSERTED WHERE THE PARSE IS, so that a route added later fires
 * instead of silently widening the old wrong answer. Asserting it at the dispatch instead is vacuous — the
 * dispatch just decided — and asserting it inside core/html/html_parse.h is wrong, because that entry serves
 * HTML §13.4 "Parsing HTML fragments", §8.5.1 `parseFromString`, §4.5.1 `createHTMLDocument` and XHR's
 * responseXML, none of which has a response or a computed type at all. The one place both facts are in hand is
 * a loader that takes the type and parses. That is this. */
#include <stdlib.h>

#include "core/html/html_parse.h"
#include "core/loader/document_load_type.h"
#include "core/loader/html_document.h"
#include "core/mime/mime_type.h"
#include "check.h"

/* WHAT A §7.5.2 LOAD IS BETWEEN TWO OF ITS NETWORKING TASKS: the Document being filled, the response's
   characters, and how far into them the input byte stream has been filled. `st` is the open's answer, held
   because a load whose open failed has no EOF to emit and `finish` is where the caller learns it. */
struct HtmlDocumentLoad {
    lxb_html_document_t *document;
    const lxb_char_t    *html;
    size_t               len, off;
    lxb_status_t         st;
};

HtmlDocumentLoad *html_document_load_begin(lxb_html_document_t *document, DomParseRootKind root_kind,
                                           HtmlScriptingMode scripting,
                                           const MimeType *type, const lxb_char_t *html, size_t size)
{
    HtmlDocumentLoad *l;

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

    l = malloc(sizeof(*l));
    CHECK(l != NULL,
          "OOM opening an HTML §7.5.2 document load — the DOM this parse produces is what every flow of this "
          "instance reads, so a load that cannot even begin is not a document with no endpoints");
    l->document = document;
    l->html     = html;
    l->len      = size;
    l->off      = 0;
    /* §7.5.2's "create an HTML parser … and associate it with document", with the input byte stream OPEN and
       holding nothing. The zero length is the point: the bytes arrive through `step`, one networking task at
       a time, exactly as the standard describes them arriving. */
    l->st = html_parse_document_open(document, root_kind, scripting, html, 0);
    return l;
}

bool html_document_load_ended(const HtmlDocumentLoad *load)
{
    DCHECK(load != NULL, "html_document_load_ended was asked of no load");
    return load->st != LXB_STATUS_OK || load->off == load->len;
}

void html_document_load_step(HtmlDocumentLoad *load)
{
    DCHECK(load != NULL, "html_document_load_step was asked of no load");
    DCHECK(!html_document_load_ended(load),
           "an HTML §7.5.2 load was stepped after the whole response had been filled into the parser's input "
           "byte stream — html_document_load_ended is what a driver's loop tests, and asking past it would "
           "read one byte off the end of the response");
    /* §7.5.2's "fill the parser's input byte stream with the fetched bytes and cause the HTML parser to
       perform the appropriate processing of the input stream", for ONE byte. In lexbor those are one call:
       the input stream is not a buffer this engine holds, it is the tokenizer's own cursor. */
    html_parse_document_write(load->document, load->html + load->off, 1);
    load->off++;
}

lxb_status_t html_document_load_finish(HtmlDocumentLoad *load)
{
    lxb_status_t st;

    DCHECK(load != NULL, "html_document_load_finish was asked of no load");
    DCHECK(html_document_load_ended(load),
           "an HTML §7.5.2 load was finished with response bytes still unfilled — §13.2.7 \"The end\"'s "
           "implied EOF character is what this emits, and emitting it over a stream that has not been given "
           "the whole response builds a document out of a prefix of it");
    st = load->st;
    /* A PARSE THAT NEVER BEGAN HAS NO END TO EMIT — `lxb_html_document_parse`'s own `goto failed`, and
       core/html/html_parse.h's close CRASHES on a document whose stream is not open. */
    if (st == LXB_STATUS_OK)
        st = html_parse_document_close(load->document);
    free(load);
    return st;
}
