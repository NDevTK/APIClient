/* HTML §7.5.2 "Loading HTML documents" — the loader HTML §7.4.5 "Populating a session history entry"'s
   load-a-document sends an HTML MIME type to. See html_document.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_LOADER_HTML_DOCUMENT_H
#define ENGINE_HOST_BROWSER_CORE_LOADER_HTML_DOCUMENT_H

#include <stddef.h>

#include <lexbor/html/html.h>

#include "core/mime/mime_type.h"
#include "solver/dom_cow.h"   /* DomParseRootKind — whose tree a parse builds, declared by whoever opens it */

/* §7.5.2's "To load an HTML document, given navigation params navigationParams", over a Document the caller
   has already created and the characters the response decoded to.
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
 * document by §7.4 rather than by anything this could dispatch on. */
lxb_status_t html_document_load(lxb_html_document_t *document, DomParseRootKind root_kind,
                                const MimeType *type, const lxb_char_t *html, size_t size);

#endif
