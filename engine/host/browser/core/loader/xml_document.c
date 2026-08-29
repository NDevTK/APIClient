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
#include <stdint.h>
#include <stdlib.h>
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

/* HTML §14.2 "Parsing XML documents"' script handling, as the crash that names it — for ONE node of the tree
   the parse just built. It is one node and not a walk because the walk is the DRIVER's: a document is
   attacker-length, so a loop over its nodes is exactly as unbounded as the loop over its constructs was, and
   the loader would have closed the parse's drive-to-completion only to open a second one behind it. */
static void xml_document_refuse_script(lxb_dom_node_t *n)
{
    const char *ns, *local;
    char nsbuf[128], lobuf[64];

    DCHECK(n != NULL, "the §14.2 script refusal was asked about no node");
    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) return;
    element_ns_and_local(lxb_dom_interface_element(n), &ns, &local, nsbuf, sizeof(nsbuf), lobuf, sizeof(lobuf));
    if (local == NULL || strcmp(local, "script") != 0) return;
    if (ns == NULL || strcmp(ns, XML_LOADER_HTML_NAMESPACE) != 0) return;
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

/* THE FOUR THINGS A §7.5.3 LOAD CAN BE DOING, in the order it can reach them. Every one of them is a LOOP over
   something the response controls, which is why each is a phase with an O(1) step rather than a stretch of a
   completing function: the constructs of [1] `document`, the top-level children a failed parse left behind,
   and the elements §14.2 has an opinion about. A phase list is what makes the loader's own state a thing a
   driver can stand in the middle of. */
enum { XML_LOAD_PARSE = 0,   /* one item of core/xml/xml_parse.h's walk */
       XML_LOAD_DISCARD,     /* one child of the partial tree a failed parse left */
       XML_LOAD_ERRDOC,      /* §7.5.3's inline report, which is O(1) */
       XML_LOAD_SCRIPTS,     /* one node of the §14.2 refusal walk */
       XML_LOAD_DONE };

/* WHAT A §7.5.3 LOAD IS BETWEEN TWO ITEMS. `parse` is live exactly while the phase is XML_LOAD_PARSE and is
   destroyed by the finish that assembles `report`; `scan` is the §14.2 walk's cursor, which is a NODE and not
   an index because the tree it walks is the one this same load built and nothing else can reach yet. */
struct XmlDocumentLoad {
    lxb_html_document_t *document;
    lxb_dom_node_t      *root;
    DomParseRootKind     root_kind;
    HtmlScriptingMode    scripting;
    XmlParse            *parse;
    lxb_dom_node_t      *scan;
    XmlParseReport       report;
    uint8_t              phase;
};

XmlDocumentLoad *xml_document_load_begin(lxb_html_document_t *document, DomParseRootKind root_kind,
                                         HtmlScriptingMode scripting,
                                         const MimeType *type, const lxb_char_t *xml, size_t size)
{
    XmlDocumentLoad *l;

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

    l = malloc(sizeof(*l));
    CHECK(l != NULL,
          "OOM opening an HTML §7.5.3 document load — the DOM this parse produces is what every flow of this "
          "instance reads, so a load that cannot even begin is not a document with no endpoints");
    l->document  = document;
    l->root      = lxb_dom_interface_node(document);
    l->root_kind = root_kind;
    l->scripting = scripting;
    l->scan      = NULL;
    l->phase     = XML_LOAD_PARSE;
    memset(&l->report, 0, sizeof(l->report));
    l->parse = xml_parse_begin(lxb_dom_interface_document(document), l->root, root_kind,
                               (const char *)xml, size);
    return l;
}

bool xml_document_load_ended(const XmlDocumentLoad *load)
{
    DCHECK(load != NULL, "xml_document_load_ended was asked of no load");
    return load->phase == XML_LOAD_DONE;
}

void xml_document_load_step(XmlDocumentLoad *load)
{
    DCHECK(load != NULL, "xml_document_load_step was asked of no load");
    DCHECK(load->phase != XML_LOAD_DONE,
           "an HTML §7.5.3 load was stepped after it had nothing left to do — xml_document_load_ended is what "
           "a driver's loop tests, and asking past it is a driver that did not");

    switch (load->phase) {
    case XML_LOAD_PARSE:
        if (!xml_parse_ended(load->parse)) {
            xml_parse_step(load->parse);
            return;
        }
        if (xml_parse_finish(load->parse, &load->report)) {
            load->parse = NULL;
            /* A WELL-FORMED PARSE, so the tree stands and §14.2 is the only question left — and only where
               §7.5.3 enables XML scripting support, which is what this loader alone does. */
            load->phase = (load->scripting == HTML_SCRIPTING_ENABLED) ? XML_LOAD_SCRIPTS : XML_LOAD_DONE;
            load->scan  = node_next_in(load->root, load->root);
            return;
        }
        load->parse = NULL;
        load->phase = XML_LOAD_DISCARD;
        return;

    case XML_LOAD_DISCARD:
        /* §7.5.3: "Error messages from the parse process (e.g., XML namespace well-formedness errors) may be
           reported inline by mutating the Document." The partial tree the parse built is what a browser
           discards before doing so — ONE top-level child per step, because [1] `document`'s `Misc*` is as
           long as the response chooses to make it. */
        if (load->root->first_child != NULL) {
            dom_cow_remove_child(load->root->first_child);
            return;
        }
        load->phase = XML_LOAD_ERRDOC;
        return;

    case XML_LOAD_ERRDOC:
        /* ONE ELEMENT AND ONE TEXT NODE — the description is the layer's own sentence, see
           core/xml/xml_parse.h — so this phase is a single step and never a loop. */
        xml_parse_error_document(lxb_dom_interface_document(load->document), load->root, load->root_kind,
                                 &load->report);
        load->phase = XML_LOAD_DONE;
        return;

    case XML_LOAD_SCRIPTS:
        if (load->scan == NULL) {
            load->phase = XML_LOAD_DONE;
            return;
        }
        xml_document_refuse_script(load->scan);
        load->scan = node_next_in(load->scan, load->root);
        return;
    }
    /* NOT A `default:` ARM — the switch is exhaustive over the phases above, so a phase added without a body
       does not compile rather than falling into a generic crash. XML_LOAD_DONE is refused by the DCHECK at the
       top, which is where a step past the end belongs. */
    DFAIL("an HTML §7.5.3 load stepped in a phase this loader does not define — the phase enum and this "
          "switch are one list and something wrote a value that is in neither");
}

lxb_status_t xml_document_load_finish(XmlDocumentLoad *load)
{
    DCHECK(load != NULL, "xml_document_load_finish was asked of no load");
    DCHECK(load->phase == XML_LOAD_DONE,
           "an HTML §7.5.3 load was finished with work left — the parse, the discard of a failed parse's "
           "partial tree, §7.5.3's inline error report and §14.2's refusal are each a phase, and finishing "
           "inside one leaves a Document the caller will read as complete");
    DCHECK(load->parse == NULL,
           "an HTML §7.5.3 load reached its end still holding an open XML parse — xml_parse_finish is what "
           "destroys the build, so a live one here is a tree build leaked with the load");
    free(load);
    /* §7.5.3 HAS NO FAILURE STATUS. An ill-formed document is not a load that failed — it is a load whose
       Document is the inline report the standard permits, which the phases above have already built. */
    return LXB_STATUS_OK;
}

void xml_document_load_abort(XmlDocumentLoad *load)
{
    DCHECK(load != NULL, "xml_document_load_abort was asked of no load");
    /* THE PHASES AFTER THE PARSE HAVE NOTHING TO ABANDON — the discard, §7.5.3's inline report and §14.2's
       refusal each act on the TREE, which this does not touch, so the only thing an abandoned §7.5.3 load
       still holds is an open tree build. */
    if (load->parse) {
        xml_parse_abort(load->parse);
        load->parse = NULL;
    }
    free(load);
}
