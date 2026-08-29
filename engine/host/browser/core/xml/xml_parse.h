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

/* ---- THE PARSE, AS A PULL ---------------------------------------------------------------------------------
 *
 * WHY THE LOOP IS THE CALLER'S HERE TOO, ONE LAYER UP FROM core/xml/xml_tree.h's. That header already argues it
 * for the tree build: an XML document is attacker-length, so the loop over its constructs is as unbounded as
 * the nesting was, and a loop that cannot stop between iterations is the drive-to-completion CLAUDE.md
 * §scheduler forbids. What this file added on top of that walk was §4.3.3's signature rule and the chain down
 * to the sentence to report — and then it drove the walk to completion inside one call, which handed the
 * unboundedness straight back to every consumer. The three things it owns are all O(1); only the loop was not.
 *
 * SO THE LOOP MOVES OUT AND NOTHING ELSE DOES. `xml_parse_begin` opens the build having consumed §4.3.3's
 * signature, `xml_parse_step` performs exactly ONE item of core/xml/xml_tree.h's walk, and `xml_parse_finish`
 * assembles the report and destroys the build. Every byte of the state is in the handle and none of it is on
 * the C stack — which is the whole of the suspension story, exactly as core/xml/xml_tree.h states it for the
 * layer below.
 *
 * THE ENTITY IS COPIED BY THE BUILD, not borrowed from the caller — `xml_tree_build_create` takes its own copy
 * and frees it at destroy — so a handle that outlives the caller's buffer is still reading its own bytes. That
 * is what makes the handle a thing a driver may hold across a suspension rather than a cursor into somebody
 * else's memory. */
typedef struct XmlParse XmlParse;

/* OPEN A PARSE of `len` bytes of an XML document entity into `doc`, appending [1] `document`'s children to
   `parent`. `kind` is core/xml/xml_tree.h's and means what solver/dom_cow.h says it means. Never returns NULL:
   an allocation failure here is fatal, for the reason `xml_tree_build_create` states — a dropped parse is a
   document the caller would go on to read as empty. §4.3.3's encoding signature is consumed HERE, which is
   where the bytes arrive and where core/xml/xml_char.h's precondition is met. */
XmlParse *xml_parse_begin(lxb_dom_document_t *doc, lxb_dom_node_t *parent, DomParseRootKind kind,
                          const char *text, size_t len);

/* HAS THE PARSE MATCHED [1] TO THE LAST BYTE, OR REPORTED A FATAL ERROR? core/xml/xml_tree.h's own answer,
   carried through: a build that has REPORTED an error is also ended, because XML §1.2 "Terminology" makes a
   fatal error the end of normal processing. It is what a driver's loop tests, and stepping past it is a
   DCHECK. */
bool xml_parse_ended(const XmlParse *p);

/* ONE ITEM OF [1] `document`'s walk, built into the tree. This is the whole of the parse's per-step cost and
   it is the ONE operation a driver repeats — the error is LATCHED into the handle rather than returned,
   because a driver that must ask after every step is a driver that can forget to, and `xml_parse_ended`
   already answers the only question the loop has. */
void xml_parse_step(XmlParse *p);

/* CLOSE THE PARSE and write `report`, which is written on every path and may not be NULL — a consumer that
   does not want the record still gets one, because "the parse failed" and "nobody looked" are the same false
   otherwise. Returns `report->ok`. The handle is destroyed; asking for a finish before `xml_parse_ended` is a
   DCHECK, because a report assembled over an unfinished walk names a position the document has not reached.
   THE TREE IS LEFT AS THE PARSE BUILT IT, PARTIAL AND ALL, and the caller discards it. Unwinding here would
   be this file deciding a consequence that is §8.5.1's, XHR's and §7.5.3's to decide — and §8.5.1's own step
   asserts the document has no child nodes before it builds its `parsererror`, so that consumer needs a
   document it can assert about rather than one this file already emptied. */
bool xml_parse_finish(XmlParse *p, XmlParseReport *report);

/* THE COMPLETE PARSE — begin, step to the end, finish — for the consumers that have no flow to yield to.
   HTML §8.5.1 `parseFromString` and XMLHttpRequest §3.6.6 "set a document response" are both reached from a
   plain C function with no step machine under it, so there is no driver for them to park in and the loop is
   the only shape available; HTML §7.5.3's loader does NOT come through here, because its driver is
   core/loader/xml_document.h's and it steps. Converting those two is converting the MEMBER first — a driver
   with no flow base cannot suspend however finely the thing beneath it is divided. */
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
