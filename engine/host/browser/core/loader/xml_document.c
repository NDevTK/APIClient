/* HTML §7.5.3 "Loading XML documents", and nothing else. See core/loader/xml_document.h.
 *
 * WHY THE SCRIPT WALK IS HERE AND NOT IN THE PARSER. HTML §14.2 "Parsing XML documents" is the section that
 * says what an XML parser does about a `script` element, and it says it does something only when XML SCRIPTING
 * SUPPORT is enabled: it sets the element's parser document, unsets its "force async", and — at the element's
 * END TAG, after a microtask checkpoint — prepares it. Of the three entries that parse XML in this engine, two
 * run with that support DISABLED and say so in their own standards (§8.5.1 `parseFromString` names it
 * outright; XHR §3.6.6's response document runs no script), so the capability is only ever owed HERE. Putting
 * the crash in core/xml/xml_tree.c would make it fire for the two consumers the standard exempts, and putting
 * a scripting FLAG in that component would give a grammar walk an opinion about a document's scripting state
 * that it has no way to be right about. So the parse is one question and this is another, asked by the one
 * caller that has to ask it.
 *
 * AND IT CRASHES RATHER THAN LEAVING THE ELEMENT INERT. A `script` in an XHTML document that this build parses
 * into a real tree and never prepares is a page whose behaviour is silently wrong — the document loads, the
 * DOM is right, and the code the page shipped does not run, with nothing anywhere to say so. That is the
 * §Offensive-programming case exactly: a capability that does not exist is honestly ABSENT and the crash is
 * what names it. */
#include <string.h>

#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/loader/document_load_type.h"
#include "core/loader/xml_document.h"
#include "core/mime/mime_type.h"
#include "core/xml/xml_parse.h"
#include "check.h"

/* DOM §1.4 "Namespaces"' HTML namespace, written out for core/xml/xml_ns.h's reason: lexbor's interning is
   case-folding and a table lookup would make this comparison depend on how the document happened to spell it. */
#define XML_LOADER_HTML_NAMESPACE "http://www.w3.org/1999/xhtml"

/* HTML §14.2 "Parsing XML documents"' script handling, as the crash that names it. One walk of the tree the
   parse just built, run only where the standard enables XML scripting support. */
static void xml_document_refuse_scripts(lxb_dom_node_t *root)
{
    lxb_dom_node_t *n;
    for (n = node_next_in(root, root); n != NULL; n = node_next_in(n, root)) {
        const char *ns, *local;
        char nsbuf[128], lobuf[64];
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        element_ns_and_local(lxb_dom_interface_element(n), &ns, &local, nsbuf, sizeof(nsbuf), lobuf, sizeof(lobuf));
        if (local == NULL || strcmp(local, "script") != 0) continue;
        if (ns == NULL || strcmp(ns, XML_LOADER_HTML_NAMESPACE) != 0) continue;
        DFAIL("HTML §14.2 \"Parsing XML documents\": this document holds a `script` element in the HTML "
              "namespace and §7.5.3 \"Loading XML documents\" runs its XML parser with XML SCRIPTING SUPPORT "
              "ENABLED — so §14.2's steps are owed and this build does not have them. BUILD THEM AT THE "
              "PARSE: when the XML parser creates a `script` element it must set the element's parser document "
              "and unset its \"force async\", and when it reaches that element's END TAG it must, after a "
              "microtask checkpoint, PREPARE the script (HTML §4.12.1 \"The script element\"'s \"prepare the "
              "script element\"). NOTE WHAT IS NOT OWED and must not be copied from the HTML parser: there is "
              "no raw-text tokenizer state in XML, so a `script` body is ordinary XML §3.1's [43] `content` "
              "and a `<![CDATA[` inside one is §2.7's [18] `CDSect` and NOT program text — core/xml/ already "
              "reads it that way, and the element's script text is the character data of its children. The "
              "two other entries that parse XML here are exempt by their own standards and must stay exempt: "
              "HTML §8.5.1 `parseFromString` creates its parser \"with XML scripting support disabled\", and "
              "XMLHttpRequest §3.6.6 \"set a document response\" runs no script. Leaving the element inert "
              "instead is a page that loads with the code it shipped silently not running");
    }
}

lxb_status_t xml_document_load(lxb_html_document_t *document, DomParseRootKind root_kind,
                               HtmlScriptingMode scripting,
                               const MimeType *type, const lxb_char_t *xml, size_t size)
{
    XmlParseReport  report;
    lxb_dom_node_t *root;

    DCHECK(document != NULL,
           "HTML §7.5.3 was reached with no Document — its own step creates one and hands it to the XML "
           "parser, so a loader with nothing to load into never ran that step");
    DCHECK(xml != NULL,
           "HTML §7.5.3 was handed a NULL pointer for the response's bytes — an empty XML entity is a zero "
           "SIZE over a real pointer, and XML §2.1's [1] `document` has an answer about one (there is no "
           "element, which is a well-formedness error and not an absent response)");
    DCHECK(document_load_type_of(type) == DOC_LOAD_XML,
           "XML's parser was reached with a response HTML §7.4.5's load-a-document sends to some other §7.5 "
           "subsection — the arm is a fact about the COMPUTED TYPE and this loader re-asks it, so this is a "
           "route into the XML parse that never asked what it fetched");

    root = lxb_dom_interface_node(document);
    if (!xml_parse_document(lxb_dom_interface_document(document), root, root_kind,
                            (const char *)xml, size, &report)) {
        /* §7.5.3: "Error messages from the parse process (e.g., XML namespace well-formedness errors) may be
           reported inline by mutating the Document." The partial tree the parse built is what a browser
           discards before doing so, and the description is the layer's own sentence — see
           core/xml/xml_parse.h. */
        lxb_dom_node_t *c, *next;
        for (c = root->first_child; c != NULL; c = next) { next = c->next; dom_cow_remove_child(c); }
        xml_parse_error_document(lxb_dom_interface_document(document), root, root_kind, &report);
        return LXB_STATUS_OK;
    }
    if (scripting == HTML_SCRIPTING_ENABLED) xml_document_refuse_scripts(root);
    return LXB_STATUS_OK;
}
