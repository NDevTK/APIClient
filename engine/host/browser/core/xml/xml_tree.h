/* THE DOM TREE AN XML DOCUMENT IS — Namespaces in XML 1.0 (Third Edition) §6 "Applying Namespaces to Elements
 * and Attributes" and §6.3 "Uniqueness of Attributes", over the items core/xml/xml_document.h's [1] `document`
 * walk reports.
 *
 * WHAT IT IS, AND WHY IT IS NOT IN ANY OF THE FILES BENEATH IT. Every component in core/xml/ decides a
 * production and BUILDS NO TREE — each says so in its own head comment — because a grammar rule is a fact
 * about BYTES and a DOM node is a fact about a Document, and a file that did both would answer a question
 * about one standard with a data structure from another. This is the file that owns the join, and it owns
 * exactly the join: it holds no scanner, no delimiter and no character class, and every sentence it can report
 * about the document's SYNTAX is one of the layers' own, carried through in the detail rather than re-worded
 * here. What it adds that no layer below could is the two rules that need a NODE to be decided over — §6's
 * expansion applied to a name at the depth the element stands at, and §6.3's uniqueness over the expanded
 * names of one element's whole attribute list, which core/xml/xml_ns.h names as owed and deliberately does not
 * decide because it sees one name at a time.
 *
 * IT IS A PULL WALK, ONE ITEM PER CALL, FOR ITS SUPPLIER'S REASON ONE LAYER UP. core/xml/xml_element.h reports
 * [43] `content`'s alternatives one at a time over an explicit heap stack rather than recursing, because [43]
 * contains [39] and C recursion would put a page's own nesting depth on the C stack (CLAUDE.md §C-stack).
 * Driving that walk to completion inside one call would hand the same unboundedness back in the other
 * direction: an XML document is attacker-length, so the LOOP over its constructs is as unbounded as the
 * nesting was, and a loop that cannot stop between iterations is the drive-to-completion §scheduler forbids.
 * So the loop is the CALLER's, and this holds every byte of its state — reader, walk, scope stack, and the
 * open element, which is the DOM tree itself rather than a second stack that could disagree with it.
 *
 * THE OPEN ELEMENT IS THE TREE AND NOT A STACK BESIDE IT. A start-tag makes the new element the insertion
 * point; an end-tag makes its PARENT the insertion point. A separate stack of open elements would be a second
 * spelling of `node->parent`, and the two could drift; keeping one means the depth this file believes in and
 * the depth the tree has are the same number, which is asserted against core/xml/xml_element.h's own count on
 * every item.
 *
 * WHY IT STOPS AT THE FIRST ERROR RATHER THAN ACCUMULATING. core/xml/xml_ns.h records a binding EVEN WHEN it
 * reports a constraint violation, so that resolution stays total and a document's errors are all of them
 * rather than the first plus a cascade — and that is right for THAT layer, which cannot know who is asking.
 * Here every consumer discards the tree on any error: HTML §8.5.1 `parseFromString`'s XML arm builds a
 * `parsererror` document in its place, XMLHttpRequest §3.6.6 "set a document response" sets `responseXML` to
 * null, and HTML §7.5.3 "Loading XML documents" has no document to display. So a SECOND error has no reader,
 * and an accumulator nothing reads is the defect CLAUDE.md §Architecture names — a value that is real,
 * asserted, and consumed by nothing. If a consumer that reports every error ever exists, the layers below
 * already keep going and this is where the change lands.
 *
 * §2.4's CHARACTER RUNS AND §4.1's REFERENCES COALESCE INTO ONE Text NODE, and that is a DOM fact rather than
 * an optimization. `<a>x&amp;y</a>` is three items from the walk — [14] `CharData`, [67] `Reference`, [14]
 * `CharData` — and one Text node whose data is `x&y`, because DOM §4.10 `Text` is a run of characters and a
 * parser that emitted three would make `firstChild.data` answer `x` for a document whose element contains
 * `x&y`. §2.7's [18] `CDSect` does NOT join that run: DOM keeps it as a `CDATASection` node, which is a
 * distinct interface a page can see with `nodeType`, so a run ends at one and a new one begins after it.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_XML_XML_TREE_H
#define ENGINE_HOST_BROWSER_CORE_XML_XML_TREE_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "core/xml/xml_document.h"
#include "core/xml/xml_ns.h"
#include "solver/dom_cow.h"   /* DomParseRootKind — whose tree this parse builds, declared by whoever opens it */

/* WHICH LAYER'S SENTENCE THE REPORT IS. Three of these name a layer and carry that layer's own answer in the
   detail beside them, which is core/xml/xml_element.h's and core/xml/xml_document.h's arrangement carried up
   unchanged: transcribing their sentences into this enum is the one way two spellings of one rule drift apart.
   The two that do NOT name a layer are the two rules this component owns, because they are the two that need a
   node or a whole attribute list to decide. Zero is OK so a caller may write `if (err)`. */
typedef enum {
    XML_TREE_OK = 0,
    XML_TREE_ERR_DOCUMENT,           /* §2.1's [1] `document` walk reported one — `detail.document` names which */
    XML_TREE_ERR_QNAME,              /* Namespaces §4's [7] `QName`: a name in this document is not one */
    XML_TREE_ERR_NAMESPACE,          /* Namespaces §3/§5/§6 reported one — `detail.ns` names which */
    XML_TREE_ERR_ATTRIBUTES_UNIQUE   /* Namespaces §6.3's "Attributes Unique", over EXPANDED names */
} XmlTreeError;

/* The sentence violated, for the well-formedness error record to report. Never NULL — XML_TREE_OK has a
   message too, and a caller that formats it has asked the wrong question. The two layer-naming values say to
   ask that layer, because its sentences are ITS to word. */
const char *xml_tree_error_message(XmlTreeError err);

/* EVERY LAYER'S OWN ANSWER, WRITTEN ON EVERY CALL — core/xml/xml_document.h's contract one level up, and an OK
   here is the same positive statement it is there, never a field a caller defaults past. `within` is that
   component's OWN detail carried through rather than flattened, so a consumer follows the chain: this error
   names the layer, `document` names its sentence, `within` names the sentence below that. */
typedef struct {
    XmlDocumentError  document;
    XmlDocumentDetail within;
    XmlNsError        ns;
} XmlTreeDetail;

typedef struct XmlTreeBuild XmlTreeBuild;

/* OPEN A BUILD over `len` bytes of a document entity, appending [1]'s children to `parent` — which is the
 * Document itself for §7.5.3 and §8.5.1, and is taken as an argument rather than derived from `doc` so that
 * the one caller who ever needs otherwise does not have to reach past this.
 *
 * `text` MUST already be UTF-8 with §4.3.3's encoding signature removed — core/xml/xml_char.h's own
 * precondition, restated here because this is the entry a consumer meets first. It must be a valid pointer
 * even when `len` is zero: an empty entity is a document that ends immediately, which §2.1's [1] has an answer
 * about (there is no element), and not the absence of a document.
 *
 * `kind` IS CONSULTED, NOT STORED-AND-IGNORED, and it decides exactly one thing: WHICH CHOKEPOINT FAMILY every
 * node this build places goes through — see `xml_tree_place_created` below, which is the one place it is asked.
 * Guessing either way is silent rather than loud, which is solver/dom_cow.h's reasoning and its argument for
 * why the fact belongs to the caller that opened the parse.
 *
 * Never returns NULL: an allocation failure here is fatal, because a dropped parse is a document the caller
 * would go on to read as empty. */
XmlTreeBuild *xml_tree_build_create(lxb_dom_document_t *doc, lxb_dom_node_t *parent, DomParseRootKind kind,
                                    const char *text, size_t len);

/* PLACE A NODE THIS PARSE JUST MADE — the ONE point at which `kind` is asked, and exported because a SECOND
 * consumer asks the identical question about the identical trees: core/xml/xml_parse.h's
 * `xml_parse_error_document` builds HTML §8.5.1's `parsererror` element and its Text child into the same root
 * this build was opened on. Two spellings of one ownership rule is how the two halves of it drift apart, and
 * this rule is one whose halves cannot drift without a use-after-free (solver/dom_cow.h, the private-tree
 * family).
 *
 * `root` is the node `xml_tree_build_create` was opened on — the tree this parse is building, which for all
 * three consumers is the Document itself. `parent` is where the node goes and is `root` or a descendant of it.
 *
 * PRIVATE takes the private-tree chokepoint and records NOTHING. The delta captures only SHARED baseline state
 * (CLAUDE.md §State isolation), and a tree the declaring operation created a statement ago is not that: routing
 * a private parse through the capturing insert puts the document's whole internal structure in the delta, and
 * recording a creation per node hands a SECOND owner to every node a placement later moves out through
 * `dom_cow_take_private` — which is where a private tree's node acquires its own owner, and the only place it
 * does. Until then the root owns them, which is what frees them if the parse is abandoned.
 *
 * SHARED takes the capturing insert AND records the creation, which is the same pair HTML §13.2.6's own writer
 * makes for a shared parse: the insert says what to put back when a delta is unapplied, and the creation says
 * what to DESTROY when that delta is discarded. Without the second, a parse into the page's own tree leaves
 * every node it built detached by the undo and freed by nobody. */
void xml_tree_place_created(lxb_dom_node_t *root, DomParseRootKind kind,
                            lxb_dom_node_t *parent, lxb_dom_node_t *node);

/* THE TWO TEARDOWNS, AND THE CALLER STATES WHICH — core/xml/xml_element.h's split, carried up to the layer
   that holds the answer. THIS BUILD'S OWN `failed` CANNOT ROUTE IT, which is the whole reason the fact has to
   come from above: `failed` is set by two errors that leave the walks in OPPOSITE states — an
   XML_TREE_ERR_DOCUMENT is the document walk's own report and stops it, while a Namespaces in XML §6 error is
   reported HERE, over a start-tag item both walks answered successfully and are still standing inside.
   `destroy` is the build that matched [1] `document` to the last byte with no error at any layer; it DCHECKs
   that, and every component under it then asserts its own completion invariant with nothing to except.
   `abandon` is every other end: a fatal error at any of the three layers, and the flow driving the parse being
   gone. It asserts no residue, because the residue is what those ends mean. */
void          xml_tree_build_destroy(XmlTreeBuild *b);
void          xml_tree_build_abandon(XmlTreeBuild *b);

/* HAS [1] MATCHED TO THE LAST BYTE OF THE ENTITY? False until the root element has closed AND the `Misc*`
   after it has been read to the end — core/xml/xml_document.h's own answer, carried through. It is what a
   caller's loop tests, and asking for another step once it is true is a DCHECK. A build that has REPORTED an
   error is also ended, because XML §1.2 "Terminology" makes a fatal error the end of normal processing. */
bool xml_tree_build_ended(const XmlTreeBuild *b);

/* WHERE THE READER STANDS, 1-based and counting NORMALIZED characters — for the error record, and valid at
   any time. On an error the reader has not advanced past the offending construct, which is what makes these
   name the mistake rather than the character after it. */
size_t xml_tree_build_line(const XmlTreeBuild *b);
size_t xml_tree_build_column(const XmlTreeBuild *b);

/* THE CHARACTER LAYER'S §1.2 LATCH. Three of the errors below this file end in "ask the character layer" —
   XML_DOCUMENT_ERR_CHARACTER and, through it, XML_ELEMENT_ERR_CHARACTER and XML_TAG_ERR_CHARACTER — and the
   reader that holds the answer is this build's. Exposed so the chain a consumer follows ends somewhere rather
   than at a component it cannot reach; XML_CHAR_OK is the positive statement that no layer latched one. */
XmlCharError xml_tree_build_character_error(const XmlTreeBuild *b);

/* THE ELEMENT WHOSE END TAG THE LAST `xml_tree_build_step` PARSED, or NULL when that step parsed some other
 * construct. It reports BOTH spellings of one boundary — §3.1 "Start-Tags, End-Tags, and Empty-Element Tags"'
 * [42] `ETag`, and [44] `EmptyElemTag`, whose own close is the second half of a single item — because §3's
 * [39] `element ::= EmptyElemTag | STag content ETag` makes them the two ways an element ends, and a consumer
 * shown only the first would miss `<script src="x"/>` entirely.
 *
 * IT IS A GRAMMAR FACT AND CARRIES NO OPINION, WHICH IS THE WHOLE REASON IT IS SHAPED THIS WAY. HTML §14.2
 * "Parsing XML documents" puts a step at this boundary — a `script` element is prepared "when the element's
 * end tag is subsequently parsed" — but only "when an XML parser with XML scripting support enabled" is
 * running, and WHICH parser that is, is a fact about the CONSUMER: HTML §7.5.3 "Loading XML documents" enables
 * it, while HTML §8.5.1 "DOMParser" creates its parser "with XML scripting support disabled" and
 * XMLHttpRequest §3.6.6 "set a document response" runs no script. A scripting flag in this walk would hand a
 * grammar rule an opinion about a document's scripting state it has no way to be right about, and a `script`
 * test here would be this file deciding which elements matter. §14.2 states the general form itself — "Between
 * the time an element's start tag is parsed and the time either the element's end tag is parsed or the parser
 * detects a well-formedness error, the user agent must act as if the element was in a stack of open elements.
 * This is used by various elements to only start certain processes once they are popped off of the stack of
 * open elements" — so the BOUNDARY is what a parser owes and WHICH elements care is asked above it.
 *
 * IT IS A STATEMENT ABOUT THE LAST STEP ONLY, cleared by every step, so a consumer that asks after two steps
 * is told about the second and never about a stale one. Before the first step it is NULL, which is the same
 * positive statement rather than an absent answer: no end tag has been parsed. */
lxb_dom_node_t *xml_tree_build_closed_element(const XmlTreeBuild *b);

/* BUILD THE NEXT CONSTRUCT INTO THE TREE. One item of core/xml/xml_document.h's walk per call — which is the
   whole of this component's suspension story, since every byte of the build's state is here and none of it is
   on the C stack.
   `*detail` is written ALWAYS. On any answer but XML_TREE_OK the build is finished and the tree is a PARTIAL
   one the caller must discard; asking for another step is a DCHECK. */
XmlTreeError xml_tree_build_step(XmlTreeBuild *b, XmlTreeDetail *detail);

#endif
