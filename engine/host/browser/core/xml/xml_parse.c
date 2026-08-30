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

/* WHAT A PARSE IS BETWEEN TWO ITEMS: the build, and the record being assembled about it. Nothing else — the
   position, the reader, the scope stack and the open element are core/xml/xml_tree.h's and stay there. */
struct XmlParse {
    XmlTreeBuild  *b;
    XmlParseReport report;
};

XmlParse *xml_parse_begin(lxb_dom_document_t *doc, lxb_dom_node_t *parent, DomParseRootKind kind,
                          const char *text, size_t len)
{
    XmlParse *p;
    size_t    off;

    DCHECK(doc != NULL && parent != NULL && text != NULL,
           "an XML parse was asked for without a Document, a node to append [1] document's children to, and a "
           "pointer to the entity — an EMPTY entity is a zero LENGTH over a real pointer");

    p = malloc(sizeof(*p));
    CHECK(p != NULL,
          "OOM opening an XML parse — a dropped parse is a document the caller would go on to read as empty, "
          "which is the same false as a document that never had one");
    memset(&p->report, 0, sizeof(p->report));
    off = xml_parse_strip_signature(text, len);
    p->b = xml_tree_build_create(doc, parent, kind, text + off, len - off);
    p->report.tree = XML_TREE_OK;
    return p;
}

bool xml_parse_ended(const XmlParse *p)
{
    DCHECK(p != NULL, "xml_parse_ended was asked of no parse");
    return xml_tree_build_ended(p->b);
}

void xml_parse_step(XmlParse *p)
{
    DCHECK(p != NULL, "xml_parse_step was asked of no parse");
    DCHECK(!xml_tree_build_ended(p->b),
           "an XML parse was stepped after [1] document matched to the last byte of the entity or reported a "
           "fatal error — xml_parse_ended is what a driver's loop tests, and asking past it is a driver that "
           "did not");
    /* THE ERROR IS LATCHED AND NOT RETURNED, and the reason is core/xml/xml_tree.h's own: a build that has
       reported an error IS ended, so a driver that tests `xml_parse_ended` needs no second question and
       cannot forget to ask one. */
    p->report.tree = xml_tree_build_step(p->b, &p->report.detail);
}

lxb_dom_node_t *xml_parse_closed_element(const XmlParse *p)
{
    DCHECK(p != NULL, "xml_parse_closed_element was asked of no parse");
    return xml_tree_build_closed_element(p->b);
}

bool xml_parse_finish(XmlParse *p, XmlParseReport *report)
{
    bool ok;

    DCHECK(p != NULL, "xml_parse_finish was asked of no parse");
    DCHECK(report != NULL,
           "an XML parse was finished without the record it writes — \"the parse failed\" and \"nobody "
           "looked\" are the same false to a consumer that did not take one");
    DCHECK(xml_tree_build_ended(p->b),
           "an XML parse was finished with items of [1] document left to build — the line and column below "
           "name where the reader STOPPED, so a report assembled here would name a position the document has "
           "not reached and call a partial tree well-formed");

    p->report.character = xml_tree_build_character_error(p->b);
    p->report.line      = xml_tree_build_line(p->b);
    p->report.column    = xml_tree_build_column(p->b);
    p->report.ok        = (p->report.tree == XML_TREE_OK);
    DCHECK(!p->report.ok || p->report.character == XML_CHAR_OK,
           "an XML parse reported a well-formed document while the character layer's §1.2 latch is set — that "
           "latch is a FATAL error, after which XML §1.2 \"Terminology\" says a processor MUST NOT continue "
           "normal processing, so the two cannot both be true");
    /* THE ONE PLACE THAT KNOWS WHICH TEARDOWN THIS IS, which is why the fact is pushed DOWN from here rather
       than re-derived at each layer: `ok` is `tree == XML_TREE_OK`, and the DCHECK above has already
       established that this build ended — so `ok` is exactly "[1] document matched to the last byte with no
       error at any of the three layers", which is the one shape core/xml/xml_tree.h lets `destroy` assert.
       AN ILL-FORMED DOCUMENT TAKES THE ABANDON PATH AND THAT IS NOT AN ERROR PATH: HTML §8.5.1 and §7.5.3
       both answer one with a `parsererror` Document, so this is an ordinary outcome whose partial tree the
       caller discards — the same thing abandonment means everywhere else in this component set. */
    if (p->report.ok) xml_tree_build_destroy(p->b);
    else              xml_tree_build_abandon(p->b);
    *report = p->report;
    ok = p->report.ok;
    free(p);
    return ok;
}

void xml_parse_abort(XmlParse *p)
{
    DCHECK(p != NULL, "xml_parse_abort was asked of no parse");
    /* ALWAYS THE ABANDON PATH, INCLUDING WHEN THE PARSE HAPPENS TO HAVE ENDED. A parse is stepped one item at
       a time precisely so a flow can be torn down between two of them, and the step that makes
       `xml_parse_ended` true RETURNS before its caller asks for the finish — so an abort legitimately arrives
       over a completed [1] document, and asserting that something was still open here would crash on the very
       interleaving the per-item drive exists to allow. */
    xml_tree_build_abandon(p->b);
    free(p);
}

bool xml_parse_document(lxb_dom_document_t *doc, lxb_dom_node_t *parent, DomParseRootKind kind,
                        const char *text, size_t len, XmlParseReport *report)
{
    XmlParse *p = xml_parse_begin(doc, parent, kind, text, len);

    while (!xml_parse_ended(p))
        xml_parse_step(p);
    return xml_parse_finish(p, report);
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
