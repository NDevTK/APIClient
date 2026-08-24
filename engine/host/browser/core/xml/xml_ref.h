/* XML 1.0 (Fifth Edition) §4.1 Character and Entity References, and §4.6 Predefined Entities — THE LAYER THAT
 * TURNS A REFERENCE INTO THE THING IT REFERS TO.
 *
 * WHAT IT IS. Three productions and one sentence. §4.1's [66] `CharRef ::= '&#' [0-9]+ ';' | '&#x'
 * [0-9a-fA-F]+ ';'` with its `[WFC: Legal Character]`, [67] `Reference ::= EntityRef | CharRef`, [68]
 * `EntityRef ::= '&' Name ';'`, and §4.6's "A set of general entities (amp, lt, gt, apos, quot) is specified
 * for this purpose ... All XML processors MUST recognize these entities whether they are declared or not."
 * It sits directly on core/xml/xml_char.h — every character it looks at has already been through §2.2's [2]
 * `Char` and §2.11's end-of-line normalization — and on core/xml/xml_name.h, whose §2.3 [4] `NameStartChar`
 * and [4a] `NameChar` are what [68]'s `Name` is scanned with.
 *
 * WHY THIS IS THE PRODUCTION BELOW THE SCANNER AND NOT PART OF IT. §2.4 Character Data and Markup: "The
 * ampersand character (&) and the left angle bracket (<) MUST NOT appear in their literal form, except when
 * used as markup delimiters, or within a comment, a processing instruction, or a CDATA section." So an `&` in
 * [43] `content` and an `&` in [10] `AttValue` are the SAME construct read by two different callers, and
 * §3.3.3 Attribute-Value Normalization's step 3 is written over its result — "For a character reference,
 * append the referenced character ... For an entity reference, recursively apply step 3 of this algorithm to
 * the replacement text of the entity". A reference layer folded into either caller would have to be written
 * twice and would then have two answers to §4.6.
 *
 * WHAT COMES BACK IS THE CHARACTER OR THE NAME, AND WHICH ONE IS A FACT ABOUT THE PRODUCTION. [66] and §4.6's
 * five resolve HERE, because their answer is fixed by this standard and by nothing the document says. Every
 * other [68] resolves at §4.2 Entity Declarations, which this engine has not built — so it comes back as a
 * BORROWED `Name` for a caller that has read a DTD to resolve, and the answer to "what if nobody has"
 * is §4.1's own `[WFC: Entity Declared]`: "In a document without any DTD ... the Name given in the entity
 * reference MUST match that in an entity declaration ... except that well-formed documents need not declare
 * any of the following entities: amp, lt, gt, apos, quot." A violation of a well-formedness constraint is a
 * fatal error (§1.2 Terminology), so an undeclared name in a DTD-less document is not a gap in this engine —
 * it is the spec's answer, and it belongs to the caller that knows whether a DTD was read. This component
 * does not guess it, which is why the name comes back rather than an error.
 *
 * THE RESULT IS DATA AND IS NEVER RE-SCANNED, which is the half of §4.6 that a naive expander gets wrong.
 * §4.4.2 Included states it with the example: the string "AT&amp;T;" expands to "AT&T;" "and the remaining
 * ampersand is not recognized as an entity-reference delimiter". §4.6 is built so that this falls out rather
 * than being a special case — "If the entities lt or amp are declared, they MUST be declared as internal
 * entities whose replacement text is a character reference to the respective character ...; the double
 * escaping is REQUIRED for these entities so that references to them produce a well-formed result." So the
 * character this returns is character DATA at the position the reference stood, full stop, and a caller that
 * fed it back to a tokenizer would make `&amp;lt;` produce a `<`, which is exactly the mistake the double
 * escaping exists to prevent.
 *
 * EVERY ERROR HERE IS FATAL AND IS RETURNED RATHER THAN ASSERTED, for core/xml/xml_char.h's reason: a
 * malformed document is a page's INPUT. §2.1 Well-Formed XML Documents makes matching the grammar the first
 * condition of well-formedness, §1.2 Terminology makes a violated well-formedness constraint a fatal error,
 * and §5.1 Validating and Non-Validating Processors requires that "Validating and non-validating processors
 * alike MUST report violations of this specification's well-formedness constraints". Report is all this layer
 * does; §1.2's "MUST NOT continue normal processing" is the CALLER's obligation, and HTML §8.5.1 The
 * DOMParser interface is the consumer that turns the report into a `parsererror` document.
 *
 * A FAILED SCAN CONSUMES NOTHING, AND THAT IS WHERE IT DIVERGES FROM THE CHARACTER LAYER ON PURPOSE.
 * xml_char.h leaves its reader standing ON the offending character, because its unit IS one character, so the
 * character that broke and the start of what broke are the same position. Here the unit is a production
 * spanning several characters and those two positions differ, so the rule that stays useful is the one about
 * the CONSTRUCT: on a §4.1 error the reader is restored byte-for-byte to the `&` it was handed, so the
 * position a report quotes names the reference, and a caller is never left standing inside a construct that
 * failed. THE ONE CARVE-OUT IS XML_REF_ERR_CHARACTER, and it is not a taste: restoring a saved copy would
 * also restore the reader's §1.2 fatal LATCH to XML_CHAR_OK, silently un-reporting the error the character
 * layer just detected. So that one return leaves the reader exactly as xml_char.h left it — latched, standing
 * on the offending byte. */
#ifndef APICLIENT_XML_REF_H
#define APICLIENT_XML_REF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/xml/xml_char.h"

/* WHICH SENTENCE OF THE STANDARD A REFERENCE VIOLATED. One value per sentence, for core/xml/xml_ns.h's reason:
   an `&` that begins nothing, a `&#;` with no digits, a reference nobody closed and a reference to a code
   point that is not a character are four different mistakes an author has to be told apart. Zero is OK so a
   caller may write `if (err)`. */
typedef enum {
    XML_REF_OK = 0,
    XML_REF_ERR_NOT_A_REFERENCE,   /* §2.4: a literal `&` that begins neither [66] nor [68] */
    XML_REF_ERR_NO_DIGITS,         /* [66]: `'&#' [0-9]+` and `'&#x' [0-9a-fA-F]+` are one-or-more */
    XML_REF_ERR_UNTERMINATED,      /* [66]/[68]: the `;` that closes the reference is not there */
    XML_REF_ERR_LEGAL_CHARACTER,   /* [66] [WFC: Legal Character]: the referenced code point is not [2] Char */
    XML_REF_ERR_CHARACTER          /* the character layer latched one — ask xml_char_error_message(r->fatal) */
} XmlRefError;

/* The sentence violated, for the well-formedness error record to report. Never NULL — XML_REF_OK has a message
   too, and a caller that formats it has asked the wrong question. XML_REF_ERR_CHARACTER's message says to ask
   the reader, because the character layer's two sentences are ITS to word and duplicating them here is how the
   two spellings drift apart. */
const char *xml_ref_error_message(XmlRefError err);

/* WHICH OF [67]'S TWO ALTERNATIVES THIS WAS, with [68] split by whether §4.6 already answers it. The split is
   not a convenience: §3.3.3 Attribute-Value Normalization's step 3 branches on exactly it — a character
   reference's character is appended AS IT IS, while an entity reference's replacement text is processed
   recursively, which is why §3.3.3's own note says a `&#xD;` in an attribute value survives as a #xD while an
   entity whose replacement text is a #xD becomes a #x20. */
typedef enum {
    XML_REF_CHAR_REF,     /* [66] CharRef */
    XML_REF_PREDEFINED,   /* [68] EntityRef whose Name is one of §4.6's five */
    XML_REF_ENTITY        /* [68] EntityRef, to be resolved against §4.2's declarations */
} XmlRefKind;

/* THE ONE FIELD THAT DOES NOT APPLY IS SET TO A VALUE NO PREDICATE ACCEPTS, never to a plausible one.
   `cp` for an XML_REF_ENTITY is XML_CHAR_EOF — the end-of-entity sentinel, which is above [2] Char's ceiling,
   so a consumer that read it would fail xml_char_is_char rather than emit U+0000 as a character the document
   contained; `name` for an XML_REF_CHAR_REF is NULL, which crashes rather than reading as the empty Name. A
   character reference HAS no Name and an unresolved entity reference HAS no character until §4.2's
   declaration is read, and those are positive statements this producer makes.
   `name` BORROWS bytes of the entity the reader was initialised over. That slice is exact and not merely
   convenient: §2.11's normalization only ever rewrites #xD, and #xD is in neither [4] NameStartChar nor [4a]
   NameChar, so no character of a Name can differ from the bytes it was decoded from. */
typedef struct {
    XmlRefKind  kind;
    uint32_t    cp;         /* XML_REF_CHAR_REF, XML_REF_PREDEFINED: the referenced character */
    const char *name;       /* XML_REF_PREDEFINED, XML_REF_ENTITY: [5] Name, borrowed from the entity */
    size_t      name_len;
} XmlRef;

/* SCAN ONE [67] Reference. The reader MUST stand on the `&` — both alternatives begin with one, so the caller
   has peeked and a reader standing anywhere else is a caller that has not, which is a DCHECK and not an error
   to report about the document.
   `*out` is written ONLY when XML_REF_OK is returned. On any other answer `*out` is untouched and — except for
   XML_REF_ERR_CHARACTER, see the head comment — the reader is byte-for-byte the one that was handed in. */
XmlRefError xml_ref_scan(XmlCharReader *r, XmlRef *out);

#endif
