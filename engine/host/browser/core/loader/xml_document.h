/* HTML §7.5.3 "Loading XML documents" — the loader HTML §7.4.5 "Populating a session history entry"'s
   load-a-document sends an XML MIME type to. See xml_document.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_LOADER_XML_DOCUMENT_H
#define ENGINE_HOST_BROWSER_CORE_LOADER_XML_DOCUMENT_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/html/html.h>

#include "core/html/html_parse.h"   /* HtmlScriptingMode — stated by whoever opens the parse */
#include "core/mime/mime_type.h"
#include "solver/dom_cow.h"         /* DomParseRootKind — whose tree a parse builds */

/* §7.5.3's "When faced with displaying an XML file inline ... user agents must follow the requirements defined
 * in XML and Namespaces in XML, XML Media Types, DOM, and other relevant specifications to create and
 * initialize a Document object document, given "xml", type, and navigationParams, and return that Document.
 * They must also create a corresponding XML parser", over a Document the caller has already created and the
 * bytes the response carried.
 *
 * IT TAKES THE COMPUTED TYPE FOR THE SAME REASON core/loader/html_document.h DOES — TO REFUSE IT. The arm is a
 * fact about the type and §7.4.5 has already decided it, so there is nothing here to branch on; the assert is
 * the closure CLAUDE.md §Browser half asks for, placed where the parse is rather than at the dispatch that
 * just decided. The defect it shuts is the one this engine has on record in BOTH directions: an XML response
 * handed to HTML §13.2's tokenizer came back as a real tree with `html` and `body` synthesised around its root
 * element and a `<![CDATA[` inside a `<script>` read as program text, failing as a JavaScript COMPILE error —
 * true about the bytes and naming the wrong subsystem for a document that was never JavaScript.
 *
 * `xml` IS BYTES AND NOT CHARACTERS, which is where this differs from §7.5.2's and §7.5.4's signatures, and
 * the difference is XML's own: §4.3.3 "Character Encoding in Entities" puts an encoding SIGNATURE in front of
 * the entity and §2.8's [23] `XMLDecl` puts an `encoding` pseudo-attribute inside it, so a caller that had
 * already decoded would have decided the question this standard reserves to the document. What this build can
 * decode is UTF-8, and core/xml/xml_parse.h is where the rest of the rule crashes by name.
 *
 * §7.5.3's other three obligations are NOT this file's and are named so they are not mistaken for absent: the
 * link-header processing is the networking task source's, "scripts may run for the newly-created document" is
 * §7.5.1's shared creation infrastructure, and the during-loading navigation ID is WebDriver BiDi's. What this
 * owns is the parse and what §7.5.3 says about its errors — "Error messages from the parse process (e.g., XML
 * namespace well-formedness errors) may be reported inline by mutating the Document", which is taken.
 *
 * IT IS A PULL, and this arm is where the pull was already half built. core/xml/xml_tree.h has been a walk of
 * one construct per call since it was written, for the reason it states — an XML document is attacker-length,
 * so the loop over its constructs is unbounded and must be able to stop between iterations — and the only
 * thing standing between that and a suspension was a driver that looped to the end. It loops HERE now, one
 * construct per `step`, and so do the two other unbounded walks this loader owns: discarding the partial tree
 * a failed parse left behind, and HTML §14.2 "Parsing XML documents"' refusal over the elements the parse
 * built. Three loops over response-controlled quantities, three phases, one O(1) step each — closing the first
 * and leaving the other two would have moved the drive-to-completion rather than removed it.
 *
 * `xml` IS BORROWED ONLY ACROSS `begin`: core/xml/xml_tree.h's build takes its own copy of the entity, so a
 * load outliving the caller's buffer is still reading its own bytes. `finish` has no failure status of its
 * own, because §7.5.3 defines none — an ill-formed document is a Document holding the inline report. */
typedef struct XmlDocumentLoad XmlDocumentLoad;

XmlDocumentLoad *xml_document_load_begin(lxb_html_document_t *document, DomParseRootKind root_kind,
                                         HtmlScriptingMode scripting,
                                         const MimeType *type, const lxb_char_t *xml, size_t size);
bool             xml_document_load_ended(const XmlDocumentLoad *load);
void             xml_document_load_step(XmlDocumentLoad *load);
lxb_status_t     xml_document_load_finish(XmlDocumentLoad *load);

#endif
