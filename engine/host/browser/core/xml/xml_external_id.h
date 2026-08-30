/* XML 1.0 (Fifth Edition) §4.2.2 External Entities' [75] `ExternalID` — THE POINTER OUT OF THE DOCUMENT
 * ENTITY, SCANNED AND NEVER DEREFERENCED.
 *
 * WHAT IT IS. One production over two literals: [75] `ExternalID ::= 'SYSTEM' S SystemLiteral | 'PUBLIC' S
 * PubidLiteral S SystemLiteral`. It sits on core/xml/xml_literal.h for [11] `SystemLiteral` and [12]
 * `PubidLiteral`, on core/xml/xml_char.h for [3] `S`, and on NOTHING ELSE. It is its own file rather than a
 * paragraph of core/xml/xml_doctype.h because §2.8's [28] `doctypedecl` is not its only caller in the
 * standard — §4.2's [75] is also reached from [71] `GEDecl`, [72] `PEDecl` and §4.7's [82] `NotationDecl`,
 * and a production three declarations share is one component and not three transcriptions.
 *
 * IT SCANS AND IT DOES NOT FETCH, AND THAT SEPARATION IS THE SECURITY BOUNDARY AND NOT A STAGING CHOICE.
 * §4.2.2 says a system identifier "is meant to be converted to a URI reference ... as part of the process of
 * dereferencing it", and DEREFERENCING is what turns a parsed external identifier into an XXE. This component
 * has no network edge, no URL resolver and no caller that gives it one; what a consumer does with the two
 * slices is the consumer's own decision, taken where §Attacker-sources' `safeFetch` chokepoint can see it. So
 * a system identifier reaching this component is a STRING the document contains, exactly as a `href` is.
 *
 * §4.2.2's `#` RULE IS NOT ENFORCED HERE AND core/xml/xml_literal.h SAYS WHY. "It is an error for a fragment
 * identifier (beginning with a # character) to be part of a system identifier" is §1.2 Terminology's `error`
 * and not its `fatal error`, so reporting it as a scan failure would END a parse the standard permits to
 * continue. It belongs at the step that decides whether to dereference at all, which by the paragraph above is
 * not this one.
 *
 * WHICH LITERAL STANDS WHERE IS DECIDED BY THE KEYWORD AND BY NOTHING INSIDE THE LITERAL, which is the rule
 * core/xml/xml_literal.h states from its side. `SYSTEM` is followed by one [11]; `PUBLIC` is followed by a
 * [12] and then an [11]. The two keywords are matched by an EXACT byte compare, because §1.2 Terminology's
 * `match` performs no case folding — `<!DOCTYPE a system "x">` names no production of [75] — and because
 * every character of both is ASCII, so none can occur as a continuation byte of some other code point.
 *
 * EVERY ERROR IS FATAL AND IS RETURNED RATHER THAN ASSERTED, AND A FAILED SCAN CONSUMES NOTHING — both for
 * core/xml/xml_literal.h's reasons and with its single carve-out: an error the character layer latched is NOT
 * rewound, because restoring a saved reader would restore its §1.2 latch to XML_CHAR_OK and silently
 * un-report the fatal error that layer just detected. */
#ifndef APICLIENT_XML_EXTERNAL_ID_H
#define APICLIENT_XML_EXTERNAL_ID_H

#include <stdbool.h>
#include <stddef.h>

#include "core/xml/xml_char.h"
#include "core/xml/xml_literal.h"

/* WHICH SENTENCE OF THE STANDARD THIS EXTERNAL IDENTIFIER VIOLATED. One value per sentence, for
   core/xml/xml_literal.h's reason. Zero is OK so a caller may write `if (err)`.
     THERE IS NO `the keyword is not there` ANSWER, and that is the family's peek rule rather than an omission:
   `xml_external_id_at` is what reports the keyword and a scan handed a reader standing anywhere else is a
   caller that has not peeked, which is this engine's mistake and not the document's. [75] is reached from
   productions that spell it `(S ExternalID)?` — its ABSENCE is one of their legal readings, so "not here" is
   never an error this component may report. */
typedef enum {
    XML_EXTERNAL_ID_OK = 0,
    XML_EXTERNAL_ID_ERR_SPACE,      /* [75] writes S after the keyword, and again between PUBLIC's two literals */
    XML_EXTERNAL_ID_ERR_QUOTE,      /* [11]/[12] open with '"' or "'" and what stands here opens with neither */
    XML_EXTERNAL_ID_ERR_LITERAL,    /* the literal layer reported one — the XmlLiteralError beside it names which */
    XML_EXTERNAL_ID_ERR_CHARACTER   /* the character layer latched one — ask xml_char_error_message(r->fatal) */
} XmlExternalIdError;

/* The sentence violated, for the well-formedness error record to report. Never NULL — XML_EXTERNAL_ID_OK has a
   message too, and a caller that formats it has asked the wrong question. The two layer-naming values say to
   ask that layer, because its sentences are ITS to word and a second copy here is how two spellings drift
   apart. */
const char *xml_external_id_error_message(XmlExternalIdError err);

/* WHAT THE IDENTIFIER SAID. Both slices are BORROWED from the entity and are NOT §2.11-normalized, which is
   core/xml/xml_literal.h's contract carried through unchanged — a literal's content is a run of characters and
   §2.11 End-of-Line Handling stands between those bytes and the characters the standard says the production
   matched, so a consumer that materializes one asks core/xml/xml_char.h for the rule over the slice.
     `public_id` IS NULL ON THE `SYSTEM` ARM AND THAT IS A POSITIVE STATEMENT, not a field nobody wrote: [75]'s
   first alternative has no [12] in it at all, so "there is no public identifier" is what the production
   matched. An EMPTY public identifier is a different fact — `PUBLIC "" "x"` is a [12] of zero characters
   standing AT a position in the entity — and it comes back as a non-NULL pointer with a zero length.
   `system_id` is never NULL when the scan answered OK, because both alternatives end in one. */
typedef struct {
    const char *public_id;   size_t public_id_len;
    const char *system_id;   size_t system_id_len;
} XmlExternalId;

/* DOES THE READER STAND AT ONE OF [75]'s TWO KEYWORDS? THE PEEK IS THE CALLER'S AND THE SCAN ASSERTS IT, which
   is core/xml/xml_markup.h's arrangement: every production that reaches [75] spells it `(S ExternalID)?` or
   `(ExternalID | PublicID)`, so whether one may stand here at all is that production's rule rather than this
   component's. ONE predicate serves both alternatives because which of them this is has no bearing on whether
   an external identifier begins. */
bool xml_external_id_at(const XmlCharReader *r);

/* SCAN ONE [75]. The reader MUST stand where `xml_external_id_at` is true, and standing anywhere else is a
   caller that has not peeked, which is a DCHECK and not a document to report about.
   `*out` is written ONLY when XML_EXTERNAL_ID_OK is returned, and the reader is then positioned immediately
   after the closing delimiter of the system identifier. `*lit` is written ALWAYS, exactly as every detail in
   this family is: an XML_LITERAL_OK there is the POSITIVE statement that the literal layer found nothing to
   report, never a field a caller defaults past.
   On any other answer `*out` is untouched and — except when the character layer's §1.2 latch is set, see the
   head comment — the reader is byte-for-byte the one that was handed in. */
XmlExternalIdError xml_external_id_scan(XmlCharReader *r, XmlExternalId *out, XmlLiteralError *lit);

#endif
