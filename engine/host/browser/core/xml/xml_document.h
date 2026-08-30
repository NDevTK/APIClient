/* XML 1.0 (Fifth Edition) §2.1 Well-Formed XML Documents' [1] `document`, with §2.8 Prolog and Document Type
 * Declaration's [22] `prolog` and [27] `Misc` — THE WHOLE OF WHAT AN XML ENTITY IS, AND THE PLACE §4.1's
 * [WFC: Entity Declared] IS DECIDED TO APPLY.
 *
 * WHAT IT IS. Three productions over two delegations. §2.1's [1] `document ::= prolog element Misc*`, §2.8's
 * [22] `prolog ::= XMLDecl? Misc* (doctypedecl Misc*)?` and its [27] `Misc ::= Comment | PI | S`. The
 * `element` in the middle is §3's [39] and is not re-derived here for one line — core/xml/xml_element.h owns
 * it and the stack that decides [WFC: Element Type Match]; the `doctypedecl` is §2.8's [28] and is
 * core/xml/xml_doctype.h's. It sits on those two, on core/xml/xml_decl.h for [23] `XMLDecl`, on
 * core/xml/xml_markup.h for [15] and [16], and on core/xml/xml_char.h for [3] `S` — and on NOTHING ELSE. It
 * builds no tree, for core/xml/xml_element.h's reason.
 *
 * WHERE [28] MAY STAND AND HOW OFTEN IS THIS COMPONENT'S RULE, WHICH IS WHY THE PEEK IS ASKED HERE AND THE
 * PRODUCTION LIVES THERE. §2.8: "The document type declaration MUST appear before the first element in the
 * document", and [22] writes `(doctypedecl Misc*)?` — a group with no star, so AT MOST ONE. A second
 * `<!DOCTYPE` after the first, or one after the root element has opened, therefore matches no production of
 * [22] and is XML_DOCUMENT_ERR_PROLOG rather than a second declaration. A `<!` in the prolog that is NOT
 * `<!DOCTYPE` is a document's own mistake in the same way: §2.8 puts every markup declaration inside [28]'s
 * `intSubset`, so an `<!ENTITY` standing loose in the prolog matches nothing here.
 *
 * §4.1's [WFC: Entity Declared] APPLIES OR DOES NOT APPLY BY A FACT ONLY THIS COMPONENT HOLDS, AND THAT IS WHY
 * THE DECISION IS HERE. The constraint's own scope is three clauses — "In a document without any DTD, a
 * document with only an internal DTD subset which contains no parameter entity references, or a document with
 * `standalone='yes'`" — and §4.1's note says the rest outright: "non-validating processors are not obligated
 * to read and process entity declarations occurring in parameter entities or in the external subset; for such
 * documents, the rule that an entity must be declared is a well-formedness constraint only if
 * standalone='yes'." So the answer turns on [28]'s `(S ExternalID)?` conjoined with §2.9's [32] `SDDecl`, and
 * this walk is the only place both are known: it reads the [23] `XMLDecl` at offset zero and the [28] after
 * it, while core/xml/xml_element.h and core/xml/xml_tag.h can see neither.
 *   THE TWO OUTCOMES ARE NOT "STRICT" AND "LENIENT", THEY ARE "DECIDED" AND "NOT DECIDABLE HERE". With no
 * external subset — no [28] at all, or one with no [75] `ExternalID` — every declaration that exists has been
 * read (there are none), so an [68] `EntityRef` outside §4.6 Predefined Entities' five IS the violation those
 * two components report and the report stands. With an external subset and `standalone='yes'`, clause three
 * puts it back in force and the report stands again. With an external subset and no such declaration, the
 * constraint says NOTHING about that reference: reporting it ill-formed would be a well-formed document
 * reported ill-formed, and resolving it would mean dereferencing a system identifier, which is the XXE this
 * engine must not have. Neither is available, so it CRASHES, naming what would decide it.
 *   AND THE EXTERNAL SUBSET IS NEVER DEREFERENCED TO GET OUT OF THAT. §4.2.2 External Entities calls a system
 * identifier something "meant to be converted to a URI reference ... as part of the process of dereferencing
 * it", and dereferencing one from inside the engine is a network edge where CLAUDE.md §Architecture allows
 * none. What HTML §14.2 "Parsing XML documents" offers instead is a table lookup rather than a fetch: for a
 * listed set of public identifiers — the XHTML 1.0 and 1.1 DTDs among them — it names one URL whose DTD
 * declares the entities of HTML's named-character-references section, which a user agent SHOULD use. That is
 * a built-in entity set keyed on [12] `PubidLiteral`, and it is what closes the common case without reading
 * anything.
 *
 * WHERE §2.8's [23] `XMLDecl` MAY STAND IS THIS COMPONENT'S RULE, WHICH core/xml/xml_decl.h STATES FROM ITS
 * OWN SIDE. [22] writes `XMLDecl?` before `Misc*`, so a declaration is at OFFSET ZERO of the document entity
 * or nowhere — and the check is a cursor comparison rather than a flag, because the reader's own position is
 * the fact and a flag would be a second copy of it. A `<?xml` anywhere else is therefore never asked that
 * question, and what it IS is decided by §2.6: [17] `PITarget ::= Name - (('X'|'x')('M'|'m')('L'|'l'))`, so
 * it reaches the processing-instruction scan and is answered the reserved-target error, which is the correct
 * report anywhere [23] is not permitted.
 *
 * WHAT IS AN ITEM AND WHAT IS MERELY CONSUMED IS THE DOM'S QUESTION AND THE STANDARD ANSWERS IT. [27]'s
 * `Comment` and `PI` become Document children, so each is an item; so does [28], which becomes DOM §4.6's
 * `DocumentType` and is the node `document.doctype` answers with. [27]'s `S` becomes nothing, because white
 * space outside the document element is not represented in any tree an XML parser builds, and neither is [23]
 * — which is a declaration about the entity and not markup in it. The declaration is therefore held BY THE
 * WALK and asked for by name rather than delivered as an item nobody could type.
 *   THAT IS THE LINE BETWEEN [23] AND [28] AND IT IS THE DOM'S AND NOT THE GRAMMAR'S. Both are declarations
 * ABOUT the document rather than markup in it, and they are on opposite sides of this rule only because DOM
 * gives one of them an interface. [28]'s own INTERNAL SUBSET is on the [23] side of the same line and more
 * sharply: `DocumentType` has exactly three members — `name`, `publicId`, `systemId` — and no `internalSubset`
 * among them, so a subset this build read and dropped would leave nothing in the tree to say so. Its effects
 * are what a page sees, which is why core/xml/xml_doctype.h crashes at the `'['` instead.
 *
 * §2.1's "THERE IS EXACTLY ONE ELEMENT, CALLED THE ROOT" IS WHAT THE TRAILING STATE ENFORCES. After the root
 * element closes, [1] admits `Misc*` and nothing else, so a second `<` that opens an element is
 * XML_DOCUMENT_ERR_TRAILING and not a second root — the sentence is "There is exactly one element, called the
 * root, or document element, no part of which appears in the content of any other element", and a walk that
 * accepted a sibling would be answering a grammar this standard does not have.
 *
 * EVERY ERROR IS FATAL AND IS RETURNED RATHER THAN ASSERTED, AND A FAILED CALL CONSUMES NOTHING — for
 * core/xml/xml_element.h's reasons, with its unit and its one carve-out: the reader is restored to the failing
 * ITEM's first byte, except when the character layer's §1.2 latch is set, where restoring it would put that
 * latch back to XML_CHAR_OK and silently un-report a fatal error. The [WFC: Entity Declared] crash above is
 * not one of these and cannot be: it is not a document's mistake, it is a capability this build does not
 * have. */
#ifndef APICLIENT_XML_DOCUMENT_H
#define APICLIENT_XML_DOCUMENT_H

#include <stdbool.h>
#include <stddef.h>

#include "core/xml/xml_char.h"
#include "core/xml/xml_decl.h"
#include "core/xml/xml_doctype.h"
#include "core/xml/xml_element.h"
#include "core/xml/xml_markup.h"

/* WHICH SENTENCE OF THE STANDARD THIS DOCUMENT VIOLATED. One value per sentence, for core/xml/xml_ns.h's
   reason. The four that name a LAYER carry that layer's own answer in the detail beside them rather than
   transcribing its sentences into this enum. Zero is OK so a caller may write `if (err)`. */
typedef enum {
    XML_DOCUMENT_OK = 0,
    XML_DOCUMENT_ERR_NO_ELEMENT,   /* [1] document ::= prolog element Misc* — the entity holds no element */
    XML_DOCUMENT_ERR_PROLOG,       /* [22]: what stands here matches none of its constructs */
    XML_DOCUMENT_ERR_TRAILING,     /* [1]'s `Misc*` admits [27]'s three and this is none of them */
    XML_DOCUMENT_ERR_DECL,         /* §2.8's [23] layer reported one — `detail.decl` names which */
    XML_DOCUMENT_ERR_DOCTYPE,      /* §2.8's [28] layer reported one — `detail.doctype` names which */
    XML_DOCUMENT_ERR_MISC,         /* §2.5's/§2.6's layer reported one — `detail.misc` names which */
    XML_DOCUMENT_ERR_ELEMENT,      /* §3's [39] walk reported one — `detail.element` and `detail.within` */
    XML_DOCUMENT_ERR_CHARACTER     /* the character layer latched one — ask xml_char_error_message(r->fatal) */
} XmlDocumentError;

/* The sentence violated, for the well-formedness error record to report. Never NULL — XML_DOCUMENT_OK has a
   message too, and a caller that formats it has asked the wrong question. */
const char *xml_document_error_message(XmlDocumentError err);

/* EVERY LAYER'S OWN ANSWER, WRITTEN ON EVERY CALL — core/xml/xml_element.h's contract one level up, and an OK
   here is the same positive statement it is there. `within` is core/xml/xml_element.h's OWN detail, carried
   through unchanged rather than flattened: a flattening would be that component's four sentences re-spelled in
   this enum, and one rule written in two places is the defect this component set is built to avoid. So a
   consumer follows the chain — this error names the layer, `element` names its sentence, `within` names the
   sentence below that — and every link is the standard's own layering. */
typedef struct {
    XmlDeclError     decl;
    XmlDoctypeError  doctype;
    XmlDoctypeDetail doctype_within;
    XmlMarkupError   misc;
    XmlElementError  element;
    XmlElementDetail within;
} XmlDocumentDetail;

typedef struct XmlDocumentWalk XmlDocumentWalk;

/* One walk per [1] `document`. Records the running flow, on core/xml/xml_element.h's argument and with its
   assertion — the walk is flow-private C memory that no COW delta captures. */
XmlDocumentWalk *xml_document_walk_create(void);

/* THE TWO TEARDOWNS, WITH core/xml/xml_element.h'S SPLIT CARRIED THROUGH WHOLE — the caller states which,
   because neither this walk nor the one below it can know.
   `destroy` is the walk that MATCHED [1] `document` to the last byte of the entity; it DCHECKs that and
   destroys the [39] walk under it.
   `abandon` is every other end: §1.2 Terminology's fatal error reported at this layer, at the one below it or
   at the one ABOVE it (Namespaces in XML §6 is decided by the tree builder over items this walk answered
   successfully), and the flow driving the parse being gone. */
void             xml_document_walk_destroy(XmlDocumentWalk *w);
void             xml_document_walk_abandon(XmlDocumentWalk *w);

/* HAS [1] MATCHED TO THE LAST BYTE OF THE ENTITY? False until the root element has closed AND the `Misc*`
   after it has been read to the end. It is what a caller's loop tests, exactly as `xml_element_depth(w) == 0`
   is one level down, and asking for another item once it is true is a DCHECK. */
bool xml_document_ended(const XmlDocumentWalk *w);

/* §2.8's [23] `XMLDecl`, or NULL when the document carried none — which is the standard's absence, since [22]
   writes `XMLDecl?`, and not a record of defaults nobody wrote. It is asked for by name rather than delivered
   as an item because it is a declaration ABOUT the entity and becomes no node in any tree.
   ASKING BEFORE THE FIRST `xml_document_next` IS A DCHECK: until the prolog has been entered, "there is no
   declaration" and "nobody has looked yet" are the same NULL, and a consumer that could not tell them apart
   would read the second as the first. */
const XmlDecl *xml_document_declaration(const XmlDocumentWalk *w);

/* SCAN THE NEXT CONSTRUCT OF [1]. The reader MUST stand at the first byte of the document entity on the first
   call — [22] puts `XMLDecl?` at offset zero and §4.3.3's encoding signature is consumed before a character
   is read (core/xml/xml_char.h's own precondition), so a reader standing anywhere else is a caller that
   handed this walk a slice of a document rather than one.
   `*out` is written ONLY when XML_DOCUMENT_OK is returned; `*detail` is written ALWAYS. What is not an item —
   [27]'s `S`, [23]'s declaration — is consumed on the way to the one that is.
   On any answer but XML_DOCUMENT_OK the reader is byte-for-byte the one that was handed in (except when the
   character layer's §1.2 latch is set, see the head comment) and this walk is finished. */
XmlDocumentError xml_document_next(XmlDocumentWalk *w, XmlCharReader *r, XmlContentItem *out,
                                   XmlDocumentDetail *detail);

#endif
