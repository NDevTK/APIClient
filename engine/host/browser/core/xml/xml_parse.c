/* See core/xml/xml_parse.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/interfaces/text.h>

#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/xml/xml_decl.h"
#include "core/xml/xml_element.h"
#include "core/xml/xml_markup.h"
#include "core/xml/xml_parse.h"
#include "core/xml/xml_ref.h"
#include "core/xml/xml_tag.h"
#include "check.h"

/* §8.5.1's own spelling of the namespace, written once. */
#define XML_PARSERERROR_NAMESPACE "http://www.mozilla.org/newlayout/xml/parsererror.xml"

const char *xml_parse_report_message(const XmlParseReport *report)
{
    DCHECK(report != NULL, "xml_parse_report_message was asked of no report");
    if (report->ok) return "the document is well-formed and namespace-well-formed";

    switch (report->tree) {
    case XML_TREE_ERR_NAMESPACE:
        return xml_ns_error_message(report->detail.ns);
    case XML_TREE_ERR_QNAME:
    case XML_TREE_ERR_ATTRIBUTES_UNIQUE:
        return xml_tree_error_message(report->tree);
    case XML_TREE_ERR_DOCUMENT:
        break;
    case XML_TREE_OK:
        DFAIL("a parse report says the document is not well-formed and names XML_TREE_OK as the violation — "
              "`ok` and `tree` are two spellings of one fact and they disagree, which is this file's own "
              "record broken rather than anything about the document");
        return "";
    }

    /* THE CHAIN. Each layer names the layer below rather than transcribing its sentences, so this only decides
       which one to ask — see core/xml/xml_document.h and core/xml/xml_element.h, whose contract this is. */
    switch (report->detail.document) {
    case XML_DOCUMENT_ERR_DECL:
        return xml_decl_error_message(report->detail.within.decl);
    case XML_DOCUMENT_ERR_MISC:
        return xml_markup_error_message(report->detail.within.misc);
    case XML_DOCUMENT_ERR_CHARACTER:
        return xml_char_error_message(report->character);
    case XML_DOCUMENT_ERR_ELEMENT:
        switch (report->detail.within.element) {
        case XML_ELEMENT_ERR_TAG:
            /* §3.1's [10] AttValue holds [67] References, so this is the ONE combination with two non-OK
               fields — core/xml/xml_element.h says so and says the report is still the TAG's, because that is
               the construct the position names. The reference's own sentence is the more specific one. */
            if (report->detail.within.within.tag == XML_TAG_ERR_REFERENCE)
                return xml_ref_error_message(report->detail.within.within.ref);
            if (report->detail.within.within.tag == XML_TAG_ERR_CHARACTER)
                return xml_char_error_message(report->character);
            return xml_tag_error_message(report->detail.within.within.tag);
        case XML_ELEMENT_ERR_MARKUP:
            return xml_markup_error_message(report->detail.within.within.markup);
        case XML_ELEMENT_ERR_REFERENCE:
            return xml_ref_error_message(report->detail.within.within.ref);
        case XML_ELEMENT_ERR_CHARACTER:
            return xml_char_error_message(report->character);
        default:
            return xml_element_error_message(report->detail.within.element);
        }
    default:
        return xml_document_error_message(report->detail.document);
    }
}

/* §4.3.3 "Character Encoding in Entities" — the SIGNATURE half, and only its UTF-8 arm. See the header on why
   the other arms crash rather than being read as UTF-8. Returns the offset of the first byte of the entity
   proper. */
static size_t xml_parse_strip_signature(const char *text, size_t len)
{
    static const char utf8_bom[3]  = { (char)0xEF, (char)0xBB, (char)0xBF };
    static const char utf16_be[2]  = { (char)0xFE, (char)0xFF };
    static const char utf16_le[2]  = { (char)0xFF, (char)0xFE };

    if (len >= 3 && memcmp(text, utf8_bom, 3) == 0) return 3;
    if (len >= 2 && (memcmp(text, utf16_be, 2) == 0 || memcmp(text, utf16_le, 2) == 0)) {
        /* NOT a §2.2 [2] Char violation to report about the document — the document is fine and this build
           cannot read it. Reporting it as ill-formed would be a plausible diagnosis of the wrong thing. */
        CHECK_FAIL("XML §4.3.3 \"Character Encoding in Entities\": this entity carries a UTF-16 encoding "
                   "signature and this build decodes only UTF-8. BUILD THE ENCODING DETERMINATION — HTML "
                   "§7.5.3 \"Loading XML documents\" makes the actual HTTP headers the ones that must be used, "
                   "then §4.3.3's signature, then §2.8's [23] XMLDecl `encoding` pseudo-attribute, which "
                   "core/xml/xml_decl.h already reads and hands over by name — and decode to UTF-8 before "
                   "core/xml/xml_char.h, whose stated precondition is UTF-8 with the signature removed. It is "
                   "a CHECK and not a DCHECK because a release build with the crash compiled out would read "
                   "these bytes as UTF-8 and report a well-formed document as violating §2.2's [2] Char at "
                   "its first byte");
    }
    return 0;
}

bool xml_parse_document(lxb_dom_document_t *doc, lxb_dom_node_t *parent, DomParseRootKind kind,
                        const char *text, size_t len, XmlParseReport *report)
{
    XmlTreeBuild *b;
    size_t        off;

    DCHECK(report != NULL,
           "an XML parse was asked for without the record it writes — \"the parse failed\" and \"nobody "
           "looked\" are the same false to a consumer that did not take one");
    DCHECK(doc != NULL && parent != NULL && text != NULL,
           "an XML parse was asked for without a Document, a node to append [1] document's children to, and a "
           "pointer to the entity — an EMPTY entity is a zero LENGTH over a real pointer");

    memset(report, 0, sizeof(*report));
    off = xml_parse_strip_signature(text, len);
    b = xml_tree_build_create(doc, parent, kind, text + off, len - off);

    report->tree = XML_TREE_OK;
    while (!xml_tree_build_ended(b)) {
        report->tree = xml_tree_build_step(b, &report->detail);
        if (report->tree != XML_TREE_OK) break;
    }
    report->character = xml_tree_build_character_error(b);
    report->line      = xml_tree_build_line(b);
    report->column    = xml_tree_build_column(b);
    report->ok        = (report->tree == XML_TREE_OK);
    DCHECK(!report->ok || report->character == XML_CHAR_OK,
           "an XML parse reported a well-formed document while the character layer's §1.2 latch is set — that "
           "latch is a FATAL error, after which XML §1.2 \"Terminology\" says a processor MUST NOT continue "
           "normal processing, so the two cannot both be true");
    xml_tree_build_destroy(b);
    return report->ok;
}

void xml_parse_error_document(lxb_dom_document_t *doc, lxb_dom_node_t *parent, DomParseRootKind kind,
                              const XmlParseReport *report)
{
    lxb_dom_element_t *root;
    lxb_dom_text_t    *text;
    char               buf[512];
    int                n;

    DCHECK(doc != NULL && parent != NULL, "a parsererror was asked for with no Document to build it in");
    DCHECK(report != NULL, "a parsererror was asked for with no error record — HTML §8.5.1's own step is to "
                           "describe the nature of the parsing error, and there is nothing to describe");
    DCHECK(!report->ok, "a parsererror was asked for over a report of a SUCCESSFUL parse — HTML §8.5.1 builds "
                        "one only when the previous step resulted in an XML well-formedness or XML namespace "
                        "well-formedness error, so this is a claim about a failure that did not happen");

    root = element_create_ns(doc, XML_PARSERERROR_NAMESPACE, strlen(XML_PARSERERROR_NAMESPACE),
                             "parsererror", 11, NULL, 0);
    DCHECK(root != NULL, "core/dom/element.h states element_create_ns never returns NULL");
    if (kind == DOM_PARSE_ROOT_PRIVATE) dom_cow_note_created(lxb_dom_interface_node(root));
    node_insert_at(parent, lxb_dom_interface_node(root), NULL);

    /* §8.5.1's "Optionally, add attributes or children to root to describe the nature of the parsing error."
       The description is the LAYER'S OWN SENTENCE plus the position the reader stopped at — nothing is worded
       here that a component under core/xml/ has already worded. */
    n = snprintf(buf, sizeof(buf), "%s (line %zu, column %zu)",
                 xml_parse_report_message(report), report->line, report->column);
    DCHECK(n > 0, "snprintf failed formatting a parsererror description");
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    text = lxb_dom_document_create_text_node(doc, (const lxb_char_t *)buf, (size_t)n);
    CHECK(text != NULL, "OOM creating the Text child of an HTML §8.5.1 parsererror element");
    if (kind == DOM_PARSE_ROOT_PRIVATE) dom_cow_note_created(lxb_dom_interface_node(text));
    node_insert_at(lxb_dom_interface_node(root), lxb_dom_interface_node(text), NULL);
}
