/* HTML §7.5.2 "Loading HTML documents" — the loader HTML §7.4.5 "Populating a session history entry"'s
   load-a-document sends an HTML MIME type to. See html_document.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_LOADER_HTML_DOCUMENT_H
#define ENGINE_HOST_BROWSER_CORE_LOADER_HTML_DOCUMENT_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/html/html.h>

#include "core/mime/mime_type.h"
#include "solver/dom_cow.h"   /* DomParseRootKind — whose tree a parse builds, declared by whoever opens it */
#include "core/html/html_parse.h"   /* HtmlScriptingMode — HTML §13.2.4.5's flag, stated by whoever opens the parse */

/* §7.5.2's "To load an HTML document, given navigation params navigationParams", over a Document the caller
 * has already created and the characters the response decoded to — AS A PULL, because that is the shape §7.5.2
 * itself describes.
 *
 * THE STANDARD'S OWN LOAD IS INCREMENTAL AND THIS ENTRY USED TO BE A SINGLE CALL. §7.5.2: "Otherwise, create an
 * HTML parser whose allow declarative shadow roots is true and associate it with document. Each task that the
 * networking task source places on the task queue while fetching runs must then fill the parser's input byte
 * stream with the fetched bytes and cause the HTML parser to perform the appropriate processing of the input
 * stream", and then "When no more bytes are available, the user agent must queue a global task on the
 * networking task source given document's relevant global object to have the parser process the implied EOF
 * character". A task per arrival and an EOF task after them is not an implementation detail of a network stack
 * — it is the algorithm, and handing lexbor the whole response in one call collapsed every one of those tasks
 * into a single un-interruptible span the length of the document.
 *
 * SO THE LOOP IS THE DRIVER'S: this begins a core/html/html_parse.h HtmlParseRun, whose steps are §7.5.2's fills
 * and whose finish is §13.2.7 "The end"'s implied EOF; the driver steps, finishes or aborts it there.
 *
 * IT TAKES THE COMPUTED TYPE FOR ONE REASON: TO REFUSE IT. §7.5.2 itself has no type argument — §7.4.5 has
 * already decided, and there is nothing left for this to branch on — so `type` is here purely as the closure
 * CLAUDE.md §Browser half asks for, asserted where the parse is. The defect this shuts is on record and was
 * not an absent capability: three separate entries reached HTML §13.2's parser with whatever they had
 * fetched, so an XML response came back as a REAL tree with `html` and `body` synthesised around its root
 * element, and the trailing newline after that element — XML §2.1 "Well-Formed XML Documents"' `Misc`, which
 * is not in the DOM — became a text node inside the synthesised `body`. Nothing crashed. `textContent`
 * answered a string one character longer than the document contains.
 *
 * `html` IS CHARACTERS, `size` may be zero, and `root_kind` is core/html/html_parse.h's — see
 * core/loader/text_document.h, whose contract is the same one for the same reasons. §7.4's initial
 * `about:blank` does NOT come through here: it has no response, so it has no computed type, and it is an HTML
 * document by §7.4 rather than by anything this could dispatch on.
 *
 * `html` IS BORROWED FOR THE LIFE OF THE LOAD and the driver keeps it alive until `finish` returns. It is not
 * copied because lexbor's chunked tokenizer already holds pointers into whatever buffer it was handed until
 * the run terminates (core/loader/text_document.c states the same thing about its two synthetic tokens), so a
 * copy would buy nothing the caller does not already owe. */
HtmlParseRun *html_document_load_begin(lxb_html_document_t *document, DomParseRootKind root_kind,
                                       HtmlScriptingMode scripting,
                                       const MimeType *type, const lxb_char_t *html, size_t size);

#endif
