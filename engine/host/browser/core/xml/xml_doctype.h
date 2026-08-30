/* XML 1.0 (Fifth Edition) §2.8 Prolog and Document Type Declaration's [28] `doctypedecl` — THE DECLARATION
 * ITSELF, AND THE PLACE ITS INTERNAL SUBSET MUST CRASH.
 *
 * WHAT IT IS. One production over three components: [28] `doctypedecl ::= '<!DOCTYPE' S Name (S ExternalID)?
 * S? ('[' intSubset ']' S?)? '>'`. The `Name` is §2.3's [5], the `ExternalID` is §4.2.2's [75] and is
 * core/xml/xml_external_id.h's whole subject, and the bracketed group is the INTERNAL SUBSET, which is
 * §2.8's [28b] `intSubset ::= (markupdecl | DeclSep)*` over [29] `markupdecl ::= elementdecl | AttlistDecl |
 * EntityDecl | NotationDecl | PI | Comment` and [28a] `DeclSep ::= PEReference | S`. This component sits on
 * core/xml/xml_char.h for [3] `S`, core/xml/xml_name.h for [5] `Name`, core/xml/xml_external_id.h for [75],
 * and on NOTHING ELSE. It builds no node — DOM §4.6's `DocumentType` is core/xml/xml_tree.h's to place, for
 * the reason core/xml/xml_element.h gives for every other construct in this family.
 *
 * §2.8's [28b] `intSubset` IS AN UNBUILT CAPABILITY AND THE `'['` IS WHERE THIS BUILD SAYS SO BY CRASHING.
 * [29] is SIX productions and not one, and the one that decides the shape of the whole subsystem is §4.2's
 * [70] `EntityDecl`: a general entity declared internally is INCLUDED (§4.4.2 Included) wherever it is
 * referenced, so reading the subset means building §4.5 Construction of Entity Replacement Text, and an
 * inclusion whose replacement text contains further references is a RECURSIVE expansion whose growth is the
 * document's to choose. That is the billion-laughs shape, and CLAUDE.md §NO BOUNDS forbids the usual answer
 * to it — an expansion cap is a cap — so what this build owes is an expansion that is a FLOW: parkable,
 * preemptible, and paged to the cold tier under RAM pressure like every other unbounded walk in this engine,
 * with the physical RAM→DISK floor as the only limit. [28a]'s `PEReference` is the second half of the same
 * problem one level up: [WFC: PEs in Internal Subset] confines parameter-entity references in the internal
 * subset to the positions BETWEEN markup declarations, and [WFC: PE Between Declarations] makes each one's
 * replacement text match [31] `extSubsetDecl` — so the subset's own text is assembled from entities before
 * [29] is read over it. NONE OF THAT IS APPROXIMATED HERE. A `'['` crashes, naming those pieces, and the
 * declaration with NO internal subset — `<!DOCTYPE html>`, `<!DOCTYPE svg PUBLIC "…" "…">` — is COMPLETE
 * rather than partial: [28] with the bracketed group absent is a whole reading of the production, and every
 * component of it is scanned and delivered.
 *   IT IS A `CHECK_FAIL` RATHER THAN A `DFAIL`, WHICH IS DECIDED BY WHAT A RELEASE BUILD WOULD OTHERWISE SAY —
 * core/xml/xml_document.h's argument, unchanged, one production further in. A DFAIL compiles to nothing
 * outside development, so the `'['` would fall through to the arm below it and be reported as [28]'s
 * unterminated declaration, about a declaration that matches [28] exactly. A plausible diagnosis of the wrong
 * thing is worse than silence. It is transition scaffolding whose only correct trajectory is to zero.
 *
 * WHAT THE `'<!DOCTYPE'` DELIMITER IS AND WHERE IT LIVES. Here, and in exactly one place in this tree —
 * core/xml/xml_document.h used to hold it beside the crash that named it, and building [28] moved the two
 * together, which is precisely what that arrangement was for. §1.2 Terminology's `match` performs no case
 * folding, so `<!doctype` is not this string and no production of [22] `prolog` matches it. The peek is the
 * CALLER's, because WHERE a document type declaration may stand is [22]'s rule and not this component's —
 * §2.8: "The document type declaration MUST appear before the first element in the document", and [22] writes
 * `(doctypedecl Misc*)?`, which also makes it AT MOST ONE. Both are the caller's to enforce and
 * core/xml/xml_document.h enforces them.
 *   A READER STANDING AT `'<!DOCTYPE'` WITH NO [3] `S` AFTER IT IS A DOCUMENT'S OWN MISTAKE AND NOT A PEEK
 * FAILURE. [28] writes `S` and not `S?` between the delimiter and the Name, so `<!DOCTYPEhtml>` is
 * XML_DOCTYPE_ERR_SPACE — and nothing else in [22] begins with these nine bytes, so there is no other
 * production for it to have been.
 *
 * WHAT A CONSUMER LEARNS THAT IS NOT A NODE: whether an EXTERNAL SUBSET exists. §2.8 — "The document type
 * declaration can point to an external subset (a special kind of external entity) containing markup
 * declarations, or can contain the markup declarations directly in an internal subset, or can do both. The
 * DTD for a document consists of both subsets taken together." A non-validating processor is not obliged to
 * read the external subset, and §4.1's [WFC: Entity Declared] turns on exactly that: its three clauses are "a
 * document without any DTD", "a document with only an internal DTD subset which contains no parameter entity
 * references", and "a document with `standalone='yes'`". So a [75] present here means the constraint may not
 * apply at all, which is a fact about the DOCUMENT and is core/xml/xml_document.h's to decide with §2.9's
 * [32] `SDDecl` beside it. This component reports the identifier and decides nothing.
 *
 * EVERY ERROR IS FATAL AND IS RETURNED RATHER THAN ASSERTED, AND A FAILED SCAN CONSUMES NOTHING — both for
 * core/xml/xml_external_id.h's reasons and with its single carve-out: an error the character layer latched is
 * NOT rewound, because restoring a saved reader would restore its §1.2 latch to XML_CHAR_OK and silently
 * un-report the fatal error that layer just detected. The `'['` crash is not one of these and cannot be: it
 * is not a document's mistake, it is a capability this build does not have. */
#ifndef APICLIENT_XML_DOCTYPE_H
#define APICLIENT_XML_DOCTYPE_H

#include <stdbool.h>
#include <stddef.h>

#include "core/xml/xml_char.h"
#include "core/xml/xml_external_id.h"
#include "core/xml/xml_literal.h"

/* WHICH SENTENCE OF THE STANDARD THIS DECLARATION VIOLATED. One value per sentence, for
   core/xml/xml_external_id.h's reason: a declaration with no name, one whose name is not a [5] Name, one
   holding something [28] has no place for, and one the entity ended in the middle of are four different
   places to look. Zero is OK so a caller may write `if (err)`. */
typedef enum {
    XML_DOCTYPE_OK = 0,
    XML_DOCTYPE_ERR_SPACE,         /* [28] writes S — not S? — between '<!DOCTYPE' and the Name */
    XML_DOCTYPE_ERR_NAME,          /* [5] Name: what stands after that space is not one */
    XML_DOCTYPE_ERR_EXTERNAL_ID,   /* §4.2.2's layer reported one — the XmlExternalIdError beside it names which */
    XML_DOCTYPE_ERR_COMPONENT,     /* [28] admits '[' or '>' here and this is neither */
    XML_DOCTYPE_ERR_UNTERMINATED,  /* [28] closes with '>' and the entity ended first */
    XML_DOCTYPE_ERR_CHARACTER      /* the character layer latched one — ask xml_char_error_message(r->fatal) */
} XmlDoctypeError;

/* The sentence violated, for the well-formedness error record to report. Never NULL — XML_DOCTYPE_OK has a
   message too, and a caller that formats it has asked the wrong question. The two layer-naming values say to
   ask that layer, because its sentences are ITS to word. */
const char *xml_doctype_error_message(XmlDoctypeError err);

/* EVERY LAYER'S OWN ANSWER, WRITTEN ON EVERY CALL — core/xml/xml_element.h's contract, and an OK here is the
   same POSITIVE statement it is there rather than a field a caller defaults past. `literal` is
   core/xml/xml_external_id.h's OWN detail carried through unchanged rather than flattened, which is this
   component set's rule: one sentence lives in one place and a consumer follows the chain to it. */
typedef struct {
    XmlExternalIdError external;
    XmlLiteralError    literal;
} XmlDoctypeDetail;

/* WHAT THE DECLARATION SAID. `name` is a BORROWED slice of the entity and is byte-exact — §2.11 End-of-Line
   Handling only ever rewrites #xD, and #xD is in neither [4] NameStartChar nor [4a] NameChar, so no character
   of a Name can differ from the bytes it was decoded from. The two identifiers inside `external` are borrowed
   too and are NOT §2.11-normalized, which is core/xml/xml_literal.h's contract carried through: a consumer
   that materializes one asks core/xml/xml_char.h for the rule over the slice.
     `has_external` IS A POSITIVE STATEMENT AND `external` IS ZEROED WHEN IT IS FALSE. [28] writes `(S
   ExternalID)?`, so ABSENT is one of the production's own readings and not a component nobody wrote — and
   core/xml/xml_external_id.h already distinguishes an absent public identifier from an empty one, so
   collapsing absence into an empty system identifier here would destroy a distinction one layer down. */
typedef struct {
    const char   *name;   size_t name_len;
    XmlExternalId external;
    bool          has_external;
} XmlDoctype;

/* DOES THE READER STAND AT `'<!DOCTYPE'`? THE PEEK IS THE CALLER'S AND THE SCAN ASSERTS IT — see the head
   comment on why WHERE and HOW OFTEN a declaration may stand are [22]'s rules rather than this component's. */
bool xml_doctype_at(const XmlCharReader *r);

/* SCAN ONE [28]. The reader MUST stand where `xml_doctype_at` is true, and standing anywhere else is a caller
   that has not peeked, which is a DCHECK and not a document to report about.
   `*out` is written ONLY when XML_DOCTYPE_OK is returned, and the reader is then positioned immediately after
   the closing `'>'`. `*detail` is written ALWAYS.
   On any other answer `*out` is untouched and — except when the character layer's §1.2 latch is set, see the
   head comment — the reader is byte-for-byte the one that was handed in. A declaration carrying an internal
   subset does not return at all: see the head comment. */
XmlDoctypeError xml_doctype_scan(XmlCharReader *r, XmlDoctype *out, XmlDoctypeDetail *detail);

#endif
