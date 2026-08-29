/* THE ENTRY THE THREE CONSUMERS OF AN XML PARSE REACH, and the error record they each do something different
 * with — HTML §8.5.1 "DOMParser"'s `parseFromString`, XMLHttpRequest §3.6.6 "set a document response", and
 * HTML §7.5.3 "Loading XML documents".
 *
 * WHY THIS IS A FILE AND NOT A LOOP AT EACH OF THEM. core/xml/xml_tree.h is a PULL walk for a real reason (an
 * XML document is attacker-length, so the loop over its constructs is unbounded and must be able to stop
 * between iterations), and a pull walk needs a driver. Three drivers would be three copies of "open, step
 * until ended, discard on error" and, more to the point, three copies of §4.3.3's signature rule and three
 * chains down through the layers to find the sentence to report — and CLAUDE.md §Browser half is explicit that
 * a rule written at N callers is N rules, of which the last to be updated is the one that is wrong. So the
 * dispatch's own argument applies one layer in: the consumers ask for a Document out of some bytes, and what
 * they each own is what to DO with a failure, which is genuinely three different things.
 *
 * WHAT EACH CONSUMER DOES WITH A FAILURE, since that is the whole reason the report is returned rather than
 * acted on here. §8.5.1's XML arm builds a `parsererror` document in the tree's place — that is the one
 * consequence that constructs a node, so it is the one this file offers as a second entry. XHR §3.6.6 step 6
 * sets `responseXML` to null. §7.5.3 has no document to display and says error messages "may be reported
 * inline by mutating the Document", which is the same construction under a different section's permission.
 *
 * §4.3.3 "Character Encoding in Entities" IS ANSWERED HERE AND ONLY FOR ITS UTF-8 ARM. core/xml/xml_char.h
 * takes UTF-8 with the signature already removed and says so as a precondition; this is where that
 * precondition is met, because it is the entry the bytes arrive at. A UTF-8 signature is consumed. A UTF-16
 * signature is a document in an encoding this build cannot decode, and it CRASHES BY NAME rather than being
 * read as UTF-8 — which would not fail, it would report a §2.2 [2] Char violation at the first byte and name
 * the wrong subsystem for a document that is perfectly well-formed in its own encoding. That is the defect
 * CLAUDE.md §Browser half's dispatch rule is about, and the crash is what keeps it from happening silently.
 * The full rule — §7.5.3's "the actual HTTP headers ... are the ones that must be used when determining the
 * character encoding", then the declaration's own `encoding` pseudo-attribute — is a capability this build
 * does not have, and the crash is where it lands when it is built.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_XML_XML_PARSE_H
#define ENGINE_HOST_BROWSER_CORE_XML_XML_PARSE_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "core/xml/xml_char.h"
#include "core/xml/xml_tree.h"

/* WHAT THE PARSE FOUND. `ok` is the answer both standards' consumers branch on — HTML §8.5.1's "an XML
   well-formedness or XML namespace well-formedness error" is exactly `!ok`, which is why the two kinds are one
   flag here and two values in `tree`. Every other field is written on EVERY parse, including a successful one,
   so that a consumer reads a positive statement rather than a field it has to default: `tree` is XML_TREE_OK,
   `character` is XML_CHAR_OK, and `line`/`column` name where the parse stopped, which for a well-formed
   document is the end of the entity. */
typedef struct {
    bool          ok;
    XmlTreeError  tree;
    XmlTreeDetail detail;
    XmlCharError  character;   /* the §1.2 latch the three "ask the character layer" answers end at */
    size_t        line, column;
} XmlParseReport;

/* THE SENTENCE VIOLATED, FOLLOWED DOWN THE CHAIN TO THE LAYER THAT OWNS IT. Never NULL — a well-formed parse
   has a message too, and a consumer that formats one has asked the wrong question. Every component under
   core/xml/ words its own sentences and none of them is re-worded here; this only decides WHICH layer to ask,
   which is the question the nested detail records exist to answer. */
const char *xml_parse_report_message(const XmlParseReport *report);

/* PARSE `len` BYTES OF AN XML DOCUMENT ENTITY INTO `doc`, appending [1] `document`'s children to `parent`.
   Returns `report->ok`. `report` is written on every path and may not be NULL — a consumer that does not want
   the record still gets one, because "the parse failed" and "nobody looked" are the same false otherwise.
   `kind` is core/xml/xml_tree.h's and means what solver/dom_cow.h says it means.
   THE TREE IS LEFT AS THE PARSE BUILT IT, PARTIAL AND ALL, and the caller discards it. Unwinding here would
   be this file deciding a consequence that is §8.5.1's, XHR's and §7.5.3's to decide — and §8.5.1's own step
   asserts the document has no child nodes before it builds its `parsererror`, so that consumer needs a
   document it can assert about rather than one this file already emptied. */
bool xml_parse_document(lxb_dom_document_t *doc, lxb_dom_node_t *parent, DomParseRootKind kind,
                        const char *text, size_t len, XmlParseReport *report);

/* HTML §8.5.1's step 3, third bullet: "Let root be the result of creating an element given document,
   `parsererror`, and `http://www.mozilla.org/newlayout/xml/parsererror.xml`. Optionally, add attributes or
   children to root to describe the nature of the parsing error. Append root to document."
   The standard's "optionally" is taken: the element carries a Text child naming the layer's own sentence and
   the line and column the reader stopped at, because a `parsererror` with no description is the stub §NO STUBS
   forbids — a shape a page can see and learn nothing from. `report` MUST be one this file wrote and MUST NOT
   be `ok`; both are asserted, since building a parsererror for a parse that succeeded is a claim about a
   failure that did not happen. It does NOT assert §8.5.1's "document has no child nodes" — that is the
   CONSUMER's assertion about a document it created, and this entry serves §7.5.3's inline reporting too. */
void xml_parse_error_document(lxb_dom_document_t *doc, lxb_dom_node_t *parent, DomParseRootKind kind,
                              const XmlParseReport *report);

#endif
